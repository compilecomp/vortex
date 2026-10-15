// CEP:FILE: include/vortex/gc/icggc.hpp
// CEP:WHAT: The ICGGC heap substrate: card table, TLAB, the two-space
// layout (young bump + region-based old generation), the Brooks-pointer
// object header contract, the card+SATB+remembered-set write barrier, and
// young evacuation.
// CEP:WHY: docs/gc-icggc.md sections 8.1-8.3 — the allocation and barrier
// substratum every tier executes against (Rules 77-86).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: allocation paths return named Result failures (OutOfMemory
// after an idempotent evacuation attempt); the barrier is noexcept and
// never fails.
// CEP:ASSUMES: one mutator thread; 16-byte object alignment (the tagged
// pointer scheme); regions are card-aligned; the driver never overlaps an
// old cycle with a young collection.
// CEP:COST: @hot allocation fast path <= 4 cycles (add + cmp + header
// stores); the barrier <= 12 cycles steady state; card marking 1 byte.
// CEP:EVIDENCE: tests/test_gc.cpp — gc_brooks_self_after_allocation,
// gc_barriered_store_legs, gc_remset_leg_tracks_cross_region_slots,
// gc_young_evacuation_promotes_and_updates_roots.
// ICGGC — Incremental Concurrent Generational GC (docs/gc-icggc.md).
//
// M5 implements the working engine substrate: the two-space heap (young bump
// + region-based old generation), the Brooks-pointer object header, the
// card+SATB+remembered-set write barrier, young evacuation (full promotion),
// and the runtime-state hookups the collector drives. The old-generation
// cycle itself (SATB concurrent mark, sweep, evacuation, pacing) is
// IcggcEngine — include/vortex/gc/collector.hpp (docs/gc-icggc.md section 8).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "vortex/runtime/object_model.hpp"
#include "vortex/support/result.hpp"
#include "vortex/support/tagged_value.hpp"

namespace vortex::gc {

using support::Result;

/// Card table for cross-generational reference tracking
/// (docs/gc-icggc.md section 3, docs/tier-t0.md section 8):
///   card_table[obj >> CARD_SHIFT] = DIRTY
/// CEM-26 section 2: card geometry is a semantic domain — the shift, the
/// card size and the states are named once here.
constexpr uint64_t kCardShift = 9;  // 512-byte cards (heap granularity)
constexpr uint64_t kCardBytes = uint64_t{1} << kCardShift;
constexpr uint8_t kCardDirty = 1;
constexpr uint8_t kCardClean = 0;
static_assert(kCardBytes == 512,
              "card size is the write-barrier granularity; changing it is a "
              "GC-protocol event (docs/gc-icggc.md section 3)");

// CEP:WHAT: Card table for cross-generational reference tracking.
// CEP:WHY: docs/gc-icggc.md section 3: one dirty byte per 512-byte card.
// CEP:STATUS: complete
// CEP:FAILURE: mark_dirty is noexcept and bounds-checked (out-of-range stores drop).
// CEP:ASSUMES: heap-relative indexing against the whole buffer.
// CEP:COST: @hot: <= 4 cycles per mark (addr fold + guard + byte store).
// CEP:EVIDENCE: gc_barriered_store_legs asserts the dirty byte.
class CardTable {
public:
    CardTable() noexcept = default;
    CardTable(const void* heap_base, size_t heap_bytes)
        : base_(reinterpret_cast<uint64_t>(heap_base)) {
        cards_.assign(static_cast<size_t>((heap_bytes >> kCardShift) + 1),
                      kCardClean);
    }

    /// Card indexing is heap-relative: (obj - heap_base) >> kCardShift.
    /// (The docs' `card_table[obj >> CARD_SHIFT]` form assumes a reserved
    /// heap range; with an allocated heap we subtract the base.)
    // @hot — one mark per reference-valued field/array store.
    // PERF_CONTRACT:
    // BUDGET: <= 4 cycles (addr fold + guard + relaxed byte store)
    // READS: 0 (bounds derived from base_/size_, register arithmetic)
    // WRITES: 1 byte (the card; repeated stores to one object hit the same
    //         line — idempotent DIRTY, no read-modify-write)
    // BRANCHES: 2 (below-base guard, capacity guard — both predicted
    //           not-taken in a live heap)
    // CACHE: one card-table line per 512-byte span of stores
    // PERF_NOTE: plain (non-atomic) byte store is correct under the M0
    //           single-mutator contract; the M2 handshake publishes card
    //           state, no atomicity is required at the byte (docs/
    //           infrastructure/04-threading-suspension.md).
// CEP:WHAT: Marks the card covering `obj` dirty.
// CEP:WHY: the young collector's region walk uses dirty cards as its edge source.
// CEP:STATUS: complete
// CEP:FAILURE: noexcept; below-base and out-of-range stores are dropped.
// CEP:ASSUMES: obj points into this heap (or the store is dropped).
// CEP:COST: @hot: <= 4 cycles; repeated stores hit the same line idempotently.
// CEP:EVIDENCE: gc_barriered_store_legs.
    void mark_dirty(const void* obj) noexcept {
        const uint64_t addr = reinterpret_cast<uint64_t>(obj);
        if (addr < base_) return;
        const uint64_t idx = (addr - base_) >> kCardShift;
        if (idx < cards_.size()) cards_[idx] = kCardDirty;
    }
    uint8_t card(const void* obj) const noexcept {
        const uint64_t addr = reinterpret_cast<uint64_t>(obj);
        if (addr < base_) return kCardClean;
        const uint64_t idx = (addr - base_) >> kCardShift;
        return idx < cards_.size() ? cards_[idx] : kCardClean;
    }
    uint8_t* data() noexcept { return cards_.data(); }
    const uint8_t* data() const noexcept { return cards_.data(); }
    void clear_all() noexcept {
        for (auto& c : cards_) c = kCardClean;
    }
// CEP:WHAT: Clears the card covering `obj`.
// CEP:WHY: processed cards must not rescan (their edges became old->old or died).
// CEP:STATUS: complete
// CEP:FAILURE: noexcept; out-of-range is a no-op.
// CEP:ASSUMES: the caller processed the card's edges.
// CEP:COST: @cold. One byte store.
// CEP:EVIDENCE: collect_young's card cleanup.
    void clear_card(const void* obj) noexcept {
        const uint64_t addr = reinterpret_cast<uint64_t>(obj);
        if (addr < base_) return;
        const uint64_t idx = (addr - base_) >> kCardShift;
        if (idx < cards_.size()) cards_[idx] = kCardClean;
    }
    /// Index of the card covering `addr`, or SIZE_MAX when outside the table
    /// (the young collector iterates dirty old-space cards by index).
    size_t index_of(const void* addr) const noexcept {
        const uint64_t a = reinterpret_cast<uint64_t>(addr);
        if (a < base_) return SIZE_MAX;
        const uint64_t idx = (a - base_) >> kCardShift;
        return idx < cards_.size() ? static_cast<size_t>(idx) : SIZE_MAX;
    }
    uint8_t card_at(size_t idx) const noexcept {
        return idx < cards_.size() ? cards_[idx] : kCardClean;
    }
    void set_card(size_t idx, uint8_t state) noexcept {
        if (idx < cards_.size()) cards_[idx] = state;
    }
    size_t card_count() const noexcept { return cards_.size(); }
    size_t dirty_count() const noexcept {
        size_t n = 0;
        for (uint8_t c : cards_) n += (c == kCardDirty);
        return n;
    }

private:
    uint64_t base_ = 0;
    std::vector<uint8_t> cards_;
};

/// Thread-Local Allocation Buffer (docs/gc-icggc.md section 2). Fast path:
///   load TLAB top; add size; compare with end; bump and initialize header.
class Tlab {
public:
    // Heap-pointer invariant granularity (TaggedValue::heap_pointer).
    static constexpr size_t kAlignment = 16;

// CEP:WHAT: Constructs a TLAB window over [base, base+bytes).
// CEP:WHY: Rule 83 — mutators allocate through thread-local buffers.
// CEP:STATUS: complete
// CEP:FAILURE: none (the window starts empty).
// CEP:ASSUMES: base is 16-byte aligned (the tagged-pointer invariant).
// CEP:COST: @cold. Three stores.
// CEP:EVIDENCE: every heap construction (Heap ctor).
    explicit Tlab(uint8_t* base, size_t bytes) noexcept {
        base_ = base;
        top_ = base;
        end_ = base + bytes;
    }

    /// Bump allocation with header initialization. Returns nullptr when full.
    /// @hot — every NEW_OBJECT/NEW_ARRAY/box allocation takes this path.
    /// PERF_CONTRACT:
    // BUDGET: <= 4 cycles steady-state (add + cmp; placement-new of the
    ///        trivial header folds into the same store window; the Brooks
    ///        self-store rides the header store window)
    // READS: 0
    // WRITES: `bytes` into fresh TLAB memory (streaming stores; header +
    ///        brooks stores included)
    // BRANCHES: 1 (top+bytes vs end — predictable not-taken until refill)
    // CACHE: write-allocated lines in the TLAB remainder; refill is the
    ///        amortized cost (Heap::refill_tlab, warm)
// CEP:WHAT: Bump allocation with header initialization; nullptr when full.
// CEP:WHY: docs/gc-icggc.md section 2 — the allocation fast path every tier takes.
// CEP:STATUS: complete
// CEP:FAILURE: nullptr on TLAB exhaustion (the caller refills or collects; never aborts, Rule 82).
// CEP:ASSUMES: bytes are rounded to 16 so the next base stays aligned.
// CEP:COST: @hot: <= 4 cycles steady state (add + cmp + header store window).
// CEP:EVIDENCE: gc_tlab_allocation_and_cards + every allocation test.
    template <typename T>
    T* try_allocate(size_t bytes) noexcept {
        // The tagged-pointer scheme requires 16-byte-aligned object bases
        // (TaggedValue::heap_pointer masks the low nibble): every bump is
        // rounded up so the NEXT allocation stays aligned too.
        bytes = (bytes + kAlignment - 1) & ~(kAlignment - 1);
        if (top_ + bytes > end_) return nullptr;
        void* p = top_;
        top_ += bytes;
        return new (p) T();  // header default-init; placement-new per object type
    }

    uint8_t* top() const noexcept { return top_; }
    uint8_t* end() const noexcept { return end_; }
    size_t remaining() const noexcept { return static_cast<size_t>(end_ - top_); }
// CEP:WHAT: Rebases the TLAB window (refill or post-evacuation reset).
// CEP:WHY: chunked TLABs need rewindowing; the young reset restarts from the base.
// CEP:STATUS: complete
// CEP:FAILURE: none (noexcept).
// CEP:ASSUMES: the new window stays inside the young space.
// CEP:COST: @warm. Three stores.
// CEP:EVIDENCE: refill_tlab / collect_young.
    void reset(uint8_t* base, size_t bytes) noexcept {
        base_ = base;
        top_ = base;
        end_ = base + bytes;
    }
    // J1 sync: adopt an externally bumped top (same base/end window).
    void sync_top(uint8_t* top) noexcept { top_ = top; }

private:
    uint8_t* base_ = nullptr;
    uint8_t* top_ = nullptr;
    uint8_t* end_ = nullptr;
};

/// Heap statistics consumed by the pacer and the `vx stats` sink.
struct HeapStats {
    uint64_t tlab_refills = 0;
    uint64_t objects_allocated = 0;
    uint64_t bytes_allocated = 0;
    uint64_t slow_path_allocations = 0;
};

/// Old-generation region state (docs/gc-icggc.md section 8.1).
enum class RegionState : uint8_t {
    Free = 0,          // unallocated
    Bump,              // promotion/allocation target (bump top)
    Occupied,          // fully allocated, awaiting mark/sweep
    CSet,              // selected for evacuation this cycle
    HumongousStart,    // first region of one >region object (never moves)
    HumongousCont,     // continuation region of a humongous run
};

/// A remembered-set entry: a heap slot (inside `owner`) whose value points
/// INTO some other region. Evacuation of the target region updates the slot
/// through `owner`'s Brooks pointer, so an owner evacuated earlier in the
/// same cycle relocates the slot correctly (docs/gc-icggc.md 8.3/8.4).
struct RemsetEntry {
    TaggedValue* slot = nullptr;
    HeapObject* owner = nullptr;
    uint64_t owner_epoch = 0;  // the owner region's epoch at log time
};

/// One old-generation region. Old space is never freed per-object: dead
/// objects stay until the whole region is reclaimed (Sweep) or evacuated
/// (CSet), so each region's object chain is a contiguous, walkable sequence
/// of [start, start+size) bodies (docs/gc-icggc.md 8.1).
struct Region {
    // Region geometry is a semantic domain (CEM-26 section 2): the size is
    // named once and static_asserted against the card granularity.
    static constexpr uint64_t kSizeLog2 = 18;               // 256 KiB
    static constexpr uint64_t kSize = uint64_t{1} << kSizeLog2;
    static constexpr uint64_t kMask = kSize - 1;

    uint8_t* start = nullptr;
    uint8_t* top = nullptr;      // bump top (Bump regions)
    uint64_t live_bytes = 0;     // from the last completed mark
    uint32_t index = 0;
    RegionState state = RegionState::Free;
    /// Bumped on every reset/reuse; remembered-set entries carry the epoch
    /// they were logged under and are dropped when it drifted (docs/
    /// gc-icggc.md 8.3 — stale slots never dereference reused memory).
    uint64_t epoch = 1;
    /// Slots in OTHER old regions referencing into this one (the barrier's
    /// remembered-set leg, docs/gc-icggc.md 8.3); evacuation updates them.
    std::vector<RemsetEntry> remset;

    bool contains(const void* addr) const noexcept {
        const uint64_t a = reinterpret_cast<uint64_t>(addr);
        const uint64_t s = reinterpret_cast<uint64_t>(start);
        return a >= s && a < s + kSize;
    }
    /// Walks the region's object chain; returns false at the end. `cur`
    /// must be region-aligned-or-object-aligned (start on first call).
// CEP:WHAT: Walks the region's object chain by header size.
// CEP:WHY: old space is never freed per-object, so each region is a contiguous walkable chain.
// CEP:STATUS: complete
// CEP:FAILURE: returns nullptr at the chain end; a zero size stops the walk (torn-tail guard).
// CEP:ASSUMES: cur is an object base inside the region; sizes are trustworthy.
// CEP:COST: @warm. O(1) per step.
// CEP:EVIDENCE: the card walk, mark clearing, and evacuation all ride it.
    static HeapObject* next_object(HeapObject* cur, uint8_t* region_end) noexcept {
        auto* p = reinterpret_cast<uint8_t*>(cur);
        if (p >= region_end) return nullptr;
        auto* obj = reinterpret_cast<HeapObject*>(p);
        const uint64_t sz = obj->header.size;
        if (sz == 0) return nullptr;  // defensive: never loop on a torn tail
        auto* next = p + sz;
        if (next >= region_end) return nullptr;
        return reinterpret_cast<HeapObject*>(next);
    }
};
static_assert(Region::kSize % kCardBytes == 0,
              "regions are card-aligned so card -> region mapping is exact");
static_assert(Region::kSize >= Tlab::kAlignment * 1024,
              "a region holds a meaningful number of objects");

/// The mutator-visible runtime state root scanning needs (docs/gc-icggc.md
/// 8.6). Type-erased j1ctx avoids an include cycle; stack_scan.cpp casts it.
/// Set once by the runtime wiring (tests/drivers); one mutator, one active
/// binding at a time (the M0 contract).
struct GcRuntimeState {
    void* interpreter = nullptr;   // vm::Interpreter* — T0 frames
    void* j1ctx = nullptr;         // j1::J1Context* — JIT frame anchor
    void* handles = nullptr;       // infra::HandleTable* — handle roots
    std::vector<void*> pin_regions;  // infra::PinningRegion* (mutable: the
                                   // GC updates entries when a pinned young
                                   // object is promoted)
};

/// Old-generation cycle driver (docs/gc-icggc.md section 8.4) — defined in
/// include/vortex/gc/collector.hpp. The Heap owns the engine instance.
class IcggcEngine;

/// Iterates the tagged slots of a heap body (docs/gc-icggc.md 8.8: the only
/// heap shapes are the object_model types; the klass word discriminates).
/// `double_klass` is the heap's boxed-double klass — its bodies carry a raw
/// payload, no tagged slots.
// @warm — the collector's trace/verify walks; not on the mutator fast path.
// PERF_CONTRACT:
// BUDGET: O(slots) with one pointer bump per slot; callers fold their work in
// READS: the body's own lines  WRITES: 0  BRANCHES: 2 (shape dispatch)
// CACHE: body lines already resident from the header touch
// CEP:WHAT: Iterates the tagged slots of a heap body (klass-discriminated).
// CEP:WHY: docs/gc-icggc.md 8.8: the only heap shapes are the object_model types.
// CEP:STATUS: complete
// CEP:FAILURE: none (read-only walk).
// CEP:ASSUMES: header.size is trustworthy; double_klass is the heap's box klass.
// CEP:COST: @warm. O(slots), one pointer bump per slot.
// CEP:EVIDENCE: the trace, verify, card walk, and resolution passes share it.
template <typename Fn>
inline void for_each_tagged_slot(HeapObject* o, const Klass* double_klass,
                                 Fn&& fn) {
    if (o->header.klass == double_klass) return;  // boxed double: raw payload
    if (o->header.klass == nullptr) {             // array: tagged elements
        auto* a = static_cast<ArrayObject*>(o);
        for (uint32_t i = 0; i < a->length(); ++i) fn(&a->element(i));
        return;
    }
    const size_t body =
        (o->header.size - sizeof(ObjectHeader)) / kTaggedSlotBytes;
    auto* base =
        reinterpret_cast<TaggedValue*>(reinterpret_cast<uint8_t*>(o) +
                                       sizeof(ObjectHeader));
    for (size_t i = 0; i < body; ++i) fn(&base[i]);
}

/// The young+old heap (docs/gc-icggc.md section 8.1): one contiguous
/// buffer, young space first, old space carved into regions after it.
/// The card table spans the whole buffer, so JIT card arithmetic
/// ((addr - heap_base) >> kCardShift) is exact for both spaces.
class Heap {
public:
    /// Default space budgets (CEM-26 section 2: named knobs; the pacer and
    /// tests read the effective sizes from the instance).
    static constexpr size_t kDefaultYoungBytes = 1 * 1024 * 1024;
    static constexpr size_t kDefaultOldBytes = 8 * 1024 * 1024;

    Heap(size_t young_bytes = kDefaultYoungBytes,
         size_t old_bytes = kDefaultOldBytes);
    ~Heap();

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    /// Allocates a plain object with `field_count` tagged fields.
    Result<Object*> allocate_object(Klass* klass, uint32_t field_count);

    /// Allocates a boxed double.
    Result<Object*> allocate_double(double value);

    /// Allocates an array of `length` tagged elements.
    Result<ArrayObject*> allocate_array(uint32_t length);

    CardTable& card_table() noexcept { return card_table_; }
    const HeapStats& stats() const noexcept { return stats_; }
    Klass* double_klass() noexcept { return double_klass_; }

    // ---- J1 native-allocation sync points (docs/roadmap.md M1) --------------
    // J1-compiled code bumps the TLAB directly through its context copy.
    // Under the single-mutator contract the C++ helpers re-sync the snapshot
    // before any Heap::allocate_* call and read it back after, so the two
    // views of the bump pointer never diverge (helper_alloc_slow, M1).
    uint8_t* tlab_top() noexcept { return tlab_.top(); }
    uint8_t* tlab_end() noexcept { return tlab_.end(); }
    void sync_tlab_top(uint8_t* top) noexcept { tlab_.sync_top(top); }

    /// Adopts the bound JIT context's bump position into the heap's TLAB
    /// view (docs/gc-icggc.md 8.1/8.6): the stencils bump the CONTEXT copy
    /// between slow paths, so any collection-side young-chain walk must
    /// adopt first or it misses JIT-allocated objects. No-op unbound.
    void sync_tlab_from_jit() noexcept;
    /// Rewinds the bound JIT context to the heap's current window (after a
    /// young reset the JIT must resume from the new base, never reuse the
    /// recycled addresses).
    void publish_tlab_to_jit() noexcept;
    const void* heap_base() const noexcept { return buffer_.get(); }

    // ---- M5: spaces, regions, barriers (docs/gc-icggc.md section 8) --------

    uint8_t* young_base() const noexcept { return buffer_.get(); }
    uint8_t* young_end() const noexcept { return buffer_.get() + young_bytes_; }
    size_t young_bytes() const noexcept { return young_bytes_; }
    uint8_t* old_base() const noexcept { return buffer_.get() + young_bytes_; }
    size_t old_bytes() const noexcept { return old_bytes_; }

    /// The region containing `addr`, or nullptr when the address is outside
    /// the old space (young addresses and foreign pointers).
    Region* region_of(const void* addr) noexcept;
    const std::vector<Region>& regions() const noexcept { return regions_; }
    std::vector<Region>& regions_mut() noexcept { return regions_; }

    /// True when `addr` points into the young space.
    bool is_young(const void* addr) const noexcept {
        return addr >= young_base() && addr < young_end();
    }
    /// True when `addr` points anywhere in this heap.
    bool is_heap_addr(const void* addr) const noexcept {
        return addr >= buffer_.get() && addr < buffer_.get() + total_bytes();
    }
    size_t total_bytes() const noexcept { return young_bytes_ + old_bytes_; }

    /// Old-space allocation for promotion (young GC) — bumps through the
    /// given region, rolling to the next Free region when it fills.
    /// Returns nullptr when old space is exhausted (named OOM upstream).
    HeapObject* promote(size_t bytes);

    /// Humongous allocation: one object larger than a region, carved as a
    /// contiguous region run, never evacuated (docs/gc-icggc.md 8.1).
    HeapObject* allocate_humongous(size_t bytes);

    /// THE mutator write barrier (docs/gc-icggc.md section 8.3). Stores
    /// `value` into the slot `slot` that lives inside `owner`'s body:
    ///   resolve the owner through Brooks (relocate the slot when the owner
    ///   was evacuated), log the overwritten value when marking is active,
    ///   store, mark the card, refine the target region's remembered set.
    /// @hot — every reference-valued field/array store in T0 and the
    /// runtime helpers.
    // PERF_CONTRACT:
    // BUDGET: <= 12 cycles steady state (resolve load+cmp; flag load+test;
    //         slot store; card store; region fold for the remset test)
    // READS: 8 (brooks) + 8 (old value, marking only) + metadata bytes
    // WRITES: 8 (slot) + 1 (card) + rare remset append
    // BRANCHES: 3 (resolve, marking-active, cross-region test)
    // CACHE: the owner's line (already resident) + the card line
    void barriered_store(HeapObject* owner, TaggedValue* slot,
                         TaggedValue value) noexcept;

    /// The SATB flag the JIT marking-active test reads (the J1Context
    /// marking byte mirrors this; docs/gc-icggc.md 8.3). Out of line: the
    /// mirror update reaches into the bound J1Context.
    bool marking_active() const noexcept { return marking_active_; }
    void set_marking_active(bool on) noexcept;

    /// Barrier legs 2-5 for an already-resolved owner+slot (the JIT
    /// outlined barrier path — the stencil resolved the owner before
    /// computing the slot, docs/gc-icggc.md 8.3): SATB log, store, card,
    /// remset. The owner rides into the remembered-set entry so evacuation
    /// of the target region can relocate the slot later.
    void barriered_store_resolved(HeapObject* resolved_owner,
                                  TaggedValue* slot,
                                  TaggedValue value) noexcept;

    /// SATB log drain (the collector consumes it; the barrier appends).
    std::vector<uint64_t>& satb_buffer() noexcept { return satb_; }
    const std::vector<uint64_t>& satb_buffer() const noexcept { return satb_; }

    /// Runtime state for root scanning (interpreter, JIT anchor, handles,
    /// pins) — wired by the driver/tests before any collection.
    GcRuntimeState& gc_state() noexcept { return gc_state_; }
    const GcRuntimeState& gc_state() const noexcept { return gc_state_; }

    /// FFI pin registration (docs/gc-icggc.md 8.6): the collector marks
    /// pinned objects as roots and never evacuates their regions. Entries
    /// are updated in place when a pinned young object is promoted.
    void register_pinning(void* pin_region) {
        gc_state_.pin_regions.push_back(pin_region);
    }
// CEP:WHAT: Drops a registered pin region (the FFI scope ended).
// CEP:WHY: pins are roots only while registered (docs/gc-icggc.md 8.6).
// CEP:STATUS: complete
// CEP:FAILURE: unregistered names are a no-op.
// CEP:ASSUMES: the region is not mid-scan (single mutator).
// CEP:COST: @cold. Linear over the (tiny) registration list.
// CEP:EVIDENCE: gc_compaction_preserves_identity_under_ffi_pinning.
    void unregister_pinning(void* pin_region) {
        for (size_t i = 0; i < gc_state_.pin_regions.size(); ++i) {
            if (gc_state_.pin_regions[i] == pin_region) {
                gc_state_.pin_regions.erase(gc_state_.pin_regions.begin() +
                                            static_cast<long>(i));
                return;
            }
        }
    }

    /// Young collection (docs/gc-icggc.md 8.1): evacuate every live young
    /// object into old space (full promotion), update all roots and old
    /// slots through the Brooks protocol, reset the young space. Runs
    /// synchronously on the mutator thread from the allocation slow path
    /// (Rule 82) or explicitly from the driver.
    Result<void> collect_young();

    /// Old-generation collection driver (docs/gc-icggc.md 8.4) — the Heap
    /// owns the engine instance; drivers reach it through engine().
    IcggcEngine& engine() noexcept { return *engine_; }

    // ---- old-cycle region surgery (docs/gc-icggc.md 8.4) --------------------
    // The engine orchestrates phases and slices; the Heap owns the region
    // bookkeeping these primitives implement.

    /// Evacuates one CSet region: copies its marked (live) bodies to fresh
    /// regions, installs Brooks forwarding on the frozen bodies, updates
    /// every heap slot that references the region (its remembered set + the
    /// young space's slots), and resets the region to Free. Region-
    /// granularity atomicity: a region is either fully forwarded or
    /// untouched between engine slices.
    Result<void> evacuate_region(Region& r);

    /// Reclaims a fully-dead region to the free list (Sweep).
    void reclaim_region(Region& r);

    /// Reclaims a dead humongous run (head + continuation regions).
    void reclaim_humongous_run(Region& head);

    /// Clears the mark bits of a surviving region's chain (cycle teardown;
    /// the next InitialMark starts from zero).
    void clear_region_marks(Region& r);

    /// Walks the young space's object chain (the young-as-roots leg; the
    /// chain is contiguous — chunk boundaries are stride-aligned).
// CEP:WHAT: Walks the young space's object chain (the young-as-roots leg).
// CEP:WHY: docs/gc-icggc.md 8.4: allocations during an old cycle are implicitly live.
// CEP:STATUS: complete
// CEP:FAILURE: a zero header size stops the walk (defensive torn-tail guard).
// CEP:ASSUMES: the chain [young_base, tlab top) is contiguous (stride-aligned chunks).
// CEP:COST: @warm. O(young objects).
// CEP:EVIDENCE: InitialMark and evacuate_region's phase 4.
    template <typename Fn>
    void for_each_young_object(Fn&& fn) {
        uint8_t* top = tlab_.top();
        HeapObject* o = reinterpret_cast<HeapObject*>(young_base());
        while (reinterpret_cast<uint8_t*>(o) < top) {
            fn(o);
            if (o->header.size == 0) break;  // defensive: torn tail
            o = reinterpret_cast<HeapObject*>(
                reinterpret_cast<uint8_t*>(o) + o->header.size);
        }
    }

private:
    Result<void> refill_tlab(size_t min_bytes);
    void init_header(HeapObject* obj, size_t bytes) noexcept;
    /// Sweep/reset shared tail: Free state, epoch bump, remset clear, card
    /// cleanup, free-list push (docs/gc-icggc.md 8.4).
    void reset_region_to_free(Region& r);

    std::unique_ptr<uint8_t[]> buffer_;
    size_t young_bytes_;
    size_t old_bytes_;
    Tlab tlab_;
    CardTable card_table_;
    HeapStats stats_;
    Klass* double_klass_ = nullptr;

    std::vector<Region> regions_;
    std::vector<uint32_t> free_regions_;  // region indices (Free state)
    uint32_t bump_region_ = UINT32_MAX;   // current promotion target
    bool marking_active_ = false;
    std::vector<uint64_t> satb_;          // SATB log (old-value words)
    GcRuntimeState gc_state_;
    std::unique_ptr<IcggcEngine> engine_;
};

}  // namespace vortex::gc
