// CEP:FILE: src/gc/icggc.cpp
// CEP:WHAT: The ICGGC heap implementation: two-space construction, region
// bookkeeping (promotion, humongous runs), the mutator write barrier, and
// young evacuation (full promotion with root/slot updates).
// CEP:WHY: docs/gc-icggc.md sections 8.1/8.3 — Rule 82 allocation safety,
// Rule 79 barrier completeness, Rule 83 TLAB allocation.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: allocation paths return named Result failures (OutOfMemory
// after an idempotent evacuation attempt); barrier paths are noexcept.
// CEP:ASSUMES: one mutator thread; the driver never overlaps an old cycle
// with a young collection (enforced in collect_young).
// CEP:COST: @hot allocation <= 4 cycles + amortized refill; the barrier
// <= 12 cycles steady state; young GC is one mutator pause bounded by the
// live young set.
// CEP:EVIDENCE: tests/test_gc.cpp — gc_young_evacuation_promotes_and_
// updates_roots, gc_young_gc_inside_jit_allocation_keeps_parity,
// gc_old_cycle_reclaims_dead_region.
// ICGGC heap: two-space layout, region bookkeeping, the mutator write
// barrier, and young evacuation (docs/gc-icggc.md sections 8.1/8.3).
// The old-generation cycle lives in src/gc/collector.cpp; root scanning in
// src/gc/stack_scan.cpp.
#include "vortex/gc/icggc.hpp"

#include <algorithm>
#include <cstring>

#include "vortex/gc/collector.hpp"
#include "vortex/infra/ffi.hpp"
#include "vortex/infra/observability.hpp"
#include "vortex/j1/context.hpp"
#include "vortex/support/log.hpp"

namespace vortex::gc {

namespace {
constexpr size_t kTlabChunk = 256 * 1024;
constexpr size_t kAlignment = 16;

// CEM-26 section 2: the young TLAB chunk stride is a named semantic domain.
constexpr size_t kYoungChunkBytes = kTlabChunk;

inline uint8_t* align16(uint8_t* p) noexcept {
    return reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(p) + kAlignment - 1) & ~(kAlignment - 1));
}
}  // namespace

// CEP:WHAT: Constructs the two-space heap: young bump, old regions, card table, engine.
// CEP:WHY: docs/gc-icggc.md 8.1 — one contiguous buffer keeps JIT card arithmetic exact.
// CEP:STATUS: complete
// CEP:FAILURE: throws only on allocation of the backing buffer (host OOM, before guest code).
// CEP:ASSUMES: young+old are multiples of the region/card geometry.
// CEP:COST: @cold. One allocation + region table fill.
// CEP:EVIDENCE: every Fixture in tests/test_gc.cpp.
Heap::Heap(size_t young_bytes, size_t old_bytes)
    : young_bytes_(young_bytes), old_bytes_(old_bytes), tlab_(nullptr, 0),
      card_table_() {
    // Card/region geometry preconditions (docs/gc-icggc.md 8.1): a young
    // space misaligned to the card size silently shifts every old-space
    // card index; an old space misaligned to the region size truncates.
    if ((young_bytes % kCardBytes) != 0 || (old_bytes % Region::kSize) != 0) {
        support::error("icggc",
                       "heap geometry misaligned: young % kCardBytes and "
                       "old % kRegionBytes must both be zero (card indices "
                       "and region carving would silently drift)");
    }
    buffer_ = std::make_unique<uint8_t[]>(total_bytes());
    std::memset(buffer_.get(), 0, total_bytes());
    tlab_.reset(buffer_.get(), std::min(young_bytes_, kYoungChunkBytes));
    card_table_ = CardTable(buffer_.get(), total_bytes());

    // Old space: carve regions at their fixed stride from the old base.
    regions_.resize(old_bytes_ / Region::kSize);
    for (size_t i = 0; i < regions_.size(); ++i) {
        Region& r = regions_[i];
        r.index = static_cast<uint32_t>(i);
        r.start = old_base() + i * Region::kSize;
        r.epoch = 1;
        free_regions_.push_back(r.index);
    }

    // The boxed-double klass is a heap-owned singleton descriptor (it lives on
    // the C++ heap, not in the traced heap space).
    static Klass the_double_klass("double");
    double_klass_ = &the_double_klass;

    engine_ = std::make_unique<IcggcEngine>(*this);
}

Heap::~Heap() = default;

// CEP:WHAT: Maps an address to its old-space region (nullptr outside old space).
// CEP:WHY: barrier remset legs and the collector's region surgery key on it.
// CEP:STATUS: complete
// CEP:FAILURE: noexcept; young/foreign addresses yield nullptr (the callers handle it).
// CEP:ASSUMES: regions are stride-aligned from the old base.
// CEP:COST: @hot (barrier leg): 2 compares + a divide by the region constant.
// CEP:EVIDENCE: gc_remset_leg_tracks_cross_region_slots.
Region* Heap::region_of(const void* addr) noexcept {
    const uint8_t* a = static_cast<const uint8_t*>(addr);
    if (a < old_base() || a >= old_base() + old_bytes_) return nullptr;
    const size_t idx = static_cast<size_t>(a - old_base()) / Region::kSize;
    return &regions_[idx];
}

// CEM-26 section 2: every header initialization names the Brooks contract —
// a freshly allocated body is self-forwarded until an evacuation moves it.
// @hot — every allocation site (C++ allocators; the JIT fast paths mirror
// the same stores inline).
// PERF_CONTRACT:
// BUDGET: 3 stores folded into the allocation's store window
// READS: 0  WRITES: 24 (header: klass, size+flags, brooks)
// BRANCHES: 0  CACHE: the fresh object's lines (streaming stores)
// CEP:WHAT: Initializes the size word and the Brooks self-pointer.
// CEP:WHY: docs/gc-icggc.md 8.2 — a fresh body is self-forwarded until evacuated.
// CEP:STATUS: complete
// CEP:FAILURE: none (noexcept).
// CEP:ASSUMES: the caller sets klass; bytes are the rounded allocation size.
// CEP:COST: @hot: 2 stores folded into the allocation store window.
// CEP:EVIDENCE: gc_brooks_self_after_allocation.
void Heap::init_header(HeapObject* obj, size_t bytes) noexcept {
    obj->header.size = static_cast<uint32_t>(bytes);
    obj->header.brooks = obj;  // Brooks invariant: self until evacuated
}

// CEP:WHAT: Old-space bump allocation (promotion target), rolling to fresh regions.
// CEP:WHY: the young evacuator and CSet copies allocate through it.
// CEP:STATUS: complete
// CEP:FAILURE: returns nullptr when old space is exhausted (named OOM upstream).
// CEP:ASSUMES: bytes are rounded to 16; >region sizes route to humongous.
// CEP:COST: @warm. Amortized bump; region roll marks the old one Occupied.
// CEP:EVIDENCE: gc_young_evacuation_promotes_and_updates_roots.
HeapObject* Heap::promote(size_t bytes) {
    bytes = (bytes + kAlignment - 1) & ~(kAlignment - 1);
    if (bytes > Region::kSize) return allocate_humongous(bytes);
    while (bump_region_ != UINT32_MAX) {
        Region& r = regions_[bump_region_];
        uint8_t* top = align16(r.top);
        if (top + bytes <= r.start + Region::kSize) {
            r.top = top + bytes;
            auto* obj = reinterpret_cast<HeapObject*>(top);
            init_header(obj, bytes);
            return obj;
        }
        r.state = RegionState::Occupied;
        bump_region_ = UINT32_MAX;
    }
    if (free_regions_.empty()) return nullptr;
    const uint32_t idx = free_regions_.back();
    free_regions_.pop_back();
    Region& r = regions_[idx];
    r.state = RegionState::Bump;
    r.top = r.start;
    ++r.epoch;  // reuse: stale remembered-set entries must not survive
    r.remset.clear();
    bump_region_ = idx;
    return promote(bytes);
}

// CEP:WHAT: Allocates one >region object as a contiguous region run.
// CEP:WHY: docs/gc-icggc.md 8.1 — humongous objects have separate rules, never move.
// CEP:STATUS: complete
// CEP:FAILURE: returns nullptr when no contiguous run fits.
// CEP:ASSUMES: the run is carved from the address-ordered free list.
// CEP:COST: @cold (humongous is rare). Linear free-list scan.
// CEP:EVIDENCE: gc_humongous_never_moves_and_reclaims_when_dead.
HeapObject* Heap::allocate_humongous(size_t bytes) {
    const size_t run = (bytes + Region::kSize - 1) / Region::kSize;
    if (run > free_regions_.size()) return nullptr;
    // Contiguous ADDRESS run from the free set: the LIFO free list is not
    // order-stable across reclamation, so the scan tests INDEX adjacency
    // through a presence bitmap, not list adjacency (14b-A m1 — the list
    // form false-OOMs despite free capacity).
    std::vector<uint8_t> present(regions_.size(), 0);
    for (uint32_t idx : free_regions_) present[idx] = 1;
    for (size_t start = 0; start + run <= regions_.size(); ++start) {
        bool contiguous = true;
        for (size_t j = 0; j < run; ++j) {
            if (!present[start + j] ||
                regions_[start + j].state != RegionState::Free) {
                contiguous = false;
                break;
            }
        }
        if (!contiguous) continue;
        const uint32_t first = static_cast<uint32_t>(start);
        for (size_t j = 0; j < run; ++j) {
            Region& r = regions_[first + j];
            r.state = j == 0 ? RegionState::HumongousStart
                             : RegionState::HumongousCont;
            ++r.epoch;
            r.remset.clear();
        }
        // Remove the run's regions from the free list.
        free_regions_.erase(
            std::remove_if(free_regions_.begin(), free_regions_.end(),
                           [&](uint32_t idx) {
                               return idx >= first && idx < first + run;
                           }),
            free_regions_.end());
        Region& head = regions_[first];
        head.top = head.start + bytes;  // the object spans the run
        auto* obj = reinterpret_cast<HeapObject*>(head.start);
        init_header(obj, bytes);
        return obj;
    }
    return nullptr;
}

// CEP:WHAT: Flips the SATB flag and mirrors it into the bound JIT context.
// CEP:WHY: the stencils' marking-active test reads the ctx byte (docs 8.3).
// CEP:STATUS: complete
// CEP:FAILURE: noexcept.
// CEP:ASSUMES: the bound context (if any) is the ACTIVE binding.
// CEP:COST: @cold (phase flips only). One store + optional mirror store.
// CEP:EVIDENCE: gc_satb_closure_holds_with_jit_stores_during_marking.
void Heap::sync_tlab_from_jit() noexcept {
    if (gc_state_.j1ctx == nullptr) return;
    auto* ctx = static_cast<j1::J1Context*>(gc_state_.j1ctx);
    tlab_.sync_top(static_cast<uint8_t*>(ctx->tlab_top));
}

// CEP:WHAT: Rewinds the bound JIT context to the heap's current TLAB window.
// CEP:WHY: after a young reset the JIT must resume from the new base, never the recycled addresses.
// CEP:STATUS: complete
// CEP:FAILURE: none (noexcept).
// CEP:ASSUMES: called only at collection tails (single mutator).
// CEP:COST: @cold. Two stores.
// CEP:EVIDENCE: the stress suite (the chain-integrity check).
void Heap::publish_tlab_to_jit() noexcept {
    if (gc_state_.j1ctx == nullptr) return;
    auto* ctx = static_cast<j1::J1Context*>(gc_state_.j1ctx);
    ctx->tlab_top = tlab_.top();
    ctx->tlab_end = tlab_.end();
}

// CEP:WHAT: Flips the SATB flag and mirrors it into the bound JIT context.
// CEP:WHY: the stencils' marking-active test reads the ctx byte (docs 8.3).
// CEP:STATUS: complete
// CEP:FAILURE: noexcept.
// CEP:ASSUMES: the bound context (if any) is the ACTIVE binding.
// CEP:COST: @cold (phase flips only). One store + optional mirror store.
// CEP:EVIDENCE: gc_satb_closure_holds_with_jit_stores_during_marking.
void Heap::set_marking_active(bool on) noexcept {
    marking_active_ = on;
    // The bound JIT context mirrors the flag (the stencils' marking-active
    // test reads the byte — docs/gc-icggc.md 8.3).
    if (gc_state_.j1ctx != nullptr) {
        static_cast<j1::J1Context*>(gc_state_.j1ctx)->marking_active =
            on ? 1 : 0;
    }
}

// CEP:WHAT: Barrier legs 2-5 for a pre-resolved owner+slot: SATB, store, card, remset.
// CEP:WHY: docs/gc-icggc.md 8.3 — the JIT outlined barrier and internal callers share it.
// CEP:STATUS: complete
// CEP:FAILURE: noexcept; remset growth may allocate on the C++ heap (host OOM is fatal by contract).
// CEP:ASSUMES: owner+slot are already Brooks-resolved by the caller.
// CEP:COST: @hot: <= 10 cycles steady state (flag test, store, card, region test).
// CEP:EVIDENCE: gc_barriered_store_legs, gc_remset_leg_tracks_cross_region_slots.
void Heap::barriered_store_resolved(HeapObject* resolved_owner,
                                    TaggedValue* slot,
                                    TaggedValue value) noexcept {
    // 2. SATB leg: preserve the snapshot when the concurrent marker is live.
    if (marking_active_) satb_.push_back(slot->raw());
    // 3. The store itself.
    *slot = value;
    // 4. Card leg (see barriered_store).
    card_table_.mark_dirty(slot);
    // 5. Remembered-set leg (see barriered_store): old-space slots only —
    // young owners' edges are covered by the young-scan-as-roots leg (and
    // unbounded remset growth on hot young->old edges is avoided).
    if (value.is_heap_object()) {
        HeapObject* target = value.as_heap_object();
        Region* tr = region_of(target);
        Region* sr = region_of(slot);
        if (tr != nullptr && sr != nullptr && sr != tr) {
            tr->remset.push_back(
                RemsetEntry{slot, resolved_owner, sr->epoch});
        }
    }
}

// CEP:WHAT: THE mutator write barrier: Brooks resolve + all barrier legs.
// CEP:WHY: docs/gc-icggc.md 8.3 — Rule 79: every reference store executes it.
// CEP:STATUS: complete
// CEP:FAILURE: noexcept.
// CEP:ASSUMES: slot points inside owner's body.
// CEP:COST: @hot: <= 12 cycles steady state.
// CEP:EVIDENCE: T0 field/array stores and the J1/J2-J4 helper paths.
void Heap::barriered_store(HeapObject* owner, TaggedValue* slot,
                           TaggedValue value) noexcept {
    // 1. Brooks resolution: an owner evacuated earlier relocates the slot.
    HeapObject* resolved = gc_resolve(owner);
    if (resolved != owner) {
        slot = reinterpret_cast<TaggedValue*>(
            reinterpret_cast<uint8_t*>(resolved) +
            (reinterpret_cast<uint8_t*>(slot) -
             reinterpret_cast<uint8_t*>(owner)));
    }
    barriered_store_resolved(resolved, slot, value);
}

// CEP:WHAT: Allocates a plain object (young TLAB, humongous when oversize).
// CEP:WHY: Rule 82: the slow path collects, never aborts.
// CEP:STATUS: complete
// CEP:FAILURE: named OutOfMemory when the young space and old space both refuse.
// CEP:ASSUMES: klass describes the field layout (zeroed to tagged null).
// CEP:COST: @hot: fast path <= 4 cycles; refill amortized.
// CEP:EVIDENCE: gc_tlab_allocation_and_cards.
Result<Object*> Heap::allocate_object(Klass* klass, uint32_t field_count) {
    const size_t bytes =
        (sizeof(ObjectHeader) + sizeof(TaggedValue) * field_count + kAlignment - 1) &
        ~(kAlignment - 1);
    if (bytes > Region::kSize) {
        // Humongous: one object larger than a region allocates directly in
        // old space as a region run (docs/gc-icggc.md 8.1 — separate
        // allocation and collection rules; never evacuated).
        auto* raw = allocate_humongous(bytes);
        if (raw == nullptr) {
            return support::fail(support::ErrorCode::OutOfMemory,
                                 "humongous allocation exhausted old space");
        }
        raw->header.klass = klass;
        auto* obj = static_cast<Object*>(raw);
        for (uint32_t i = 0; i < field_count; ++i) {
            obj->field(i) = TaggedValue::null();
        }
        // Zero the alignment pad after the last field too: recycled memory
        // is not zeroed, and a stale heap-tagged pad word would be a ghost
        // reference for the snapshot walk and the card scanner (M6-a
        // review 15-e-a/5).
        const size_t used = sizeof(ObjectHeader) + sizeof(TaggedValue) * field_count;
        if (bytes > used) {
            std::memset(reinterpret_cast<uint8_t*>(raw) + used, 0,
                        bytes - used);
        }
        stats_.objects_allocated++;
        stats_.bytes_allocated += bytes;
        return obj;
    }
    Object* obj = tlab_.try_allocate<Object>(bytes);
    if (obj == nullptr) {
        auto refill = refill_tlab(bytes);
        if (!refill) return std::unexpected(refill.error());
        obj = tlab_.try_allocate<Object>(bytes);
        if (obj == nullptr) {
            return support::fail(support::ErrorCode::OutOfMemory,
                                 "TLAB refill too small for object");
        }
    }
    obj->header.klass = klass;
    init_header(obj, bytes);
    // Zero fields so GC maps and interpreter reads observe well-defined nulls.
    for (uint32_t i = 0; i < field_count; ++i) obj->field(i) = TaggedValue::null();
    // Same pad contract as the humongous path above: no stale words in the
    // body, ever (M6-a review 15-e-a/5).
    const size_t used =
        sizeof(ObjectHeader) + sizeof(TaggedValue) * static_cast<size_t>(field_count);
    if (bytes > used) {
        std::memset(reinterpret_cast<uint8_t*>(obj) + used, 0, bytes - used);
    }
    stats_.objects_allocated++;
    stats_.bytes_allocated += bytes;
    return obj;
}

// CEP:WHAT: Allocates a boxed double.
// CEP:WHY: the float operand path boxes payloads (T0/J1/J2-J4 parity).
// CEP:STATUS: complete
// CEP:FAILURE: named OutOfMemory on exhaustion.
// CEP:ASSUMES: the box klass is the heap's singleton.
// CEP:COST: @hot. Same contract as allocate_object.
// CEP:EVIDENCE: the T0/J1 float parity suites.
Result<Object*> Heap::allocate_double(double value) {
    // HeapDouble layout: header + double payload.
    struct HeapDouble : Object {
        double value;
    };
    const size_t bytes =
        (sizeof(ObjectHeader) + sizeof(double) + kAlignment - 1) & ~(kAlignment - 1);
    HeapDouble* obj = tlab_.try_allocate<HeapDouble>(bytes);
    if (obj == nullptr) {
        auto refill = refill_tlab(bytes);
        if (!refill) return std::unexpected(refill.error());
        obj = tlab_.try_allocate<HeapDouble>(bytes);
        if (obj == nullptr) {
            return support::fail(support::ErrorCode::OutOfMemory,
                                 "TLAB refill too small for double box");
        }
    }
    obj->header.klass = double_klass_;
    init_header(obj, bytes);
    obj->value = value;
    stats_.objects_allocated++;
    stats_.bytes_allocated += bytes;
    return obj;
}

// CEP:WHAT: Allocates a tagged array (humongous beyond a region).
// CEP:WHY: arrays carry klass == nullptr as the discriminator.
// CEP:STATUS: complete
// CEP:FAILURE: named OutOfMemory on exhaustion.
// CEP:ASSUMES: elements zeroed to tagged null.
// CEP:COST: @hot. bytes_for(length) rounded to 16.
// CEP:EVIDENCE: gc_humongous_never_moves_and_reclaims_when_dead.
Result<ArrayObject*> Heap::allocate_array(uint32_t length) {
    const size_t bytes =
        (ArrayObject::bytes_for(length) + kAlignment - 1) & ~(kAlignment - 1);
    if (bytes > Region::kSize) {
        // Humongous array: region run in old space, never evacuated.
        auto* raw = allocate_humongous(bytes);
        if (raw == nullptr) {
            return support::fail(support::ErrorCode::OutOfMemory,
                                 "humongous array exhausted old space");
        }
        auto* obj = static_cast<ArrayObject*>(raw);
        obj->header.klass = nullptr;  // array type marker (M0)
        obj->set_length(length);
        for (uint32_t i = 0; i < length; ++i) {
            obj->element(i) = TaggedValue::null();
        }
        stats_.objects_allocated++;
        stats_.bytes_allocated += bytes;
        return obj;
    }
    ArrayObject* obj = tlab_.try_allocate<ArrayObject>(bytes);
    if (obj == nullptr) {
        auto refill = refill_tlab(bytes);
        if (!refill) return std::unexpected(refill.error());
        obj = tlab_.try_allocate<ArrayObject>(bytes);
        if (obj == nullptr) {
            return support::fail(support::ErrorCode::OutOfMemory,
                                 "TLAB refill too small for array");
        }
    }
    obj->header.klass = nullptr;  // array type marker (M0)
    init_header(obj, bytes);
    obj->set_length(length);
    for (uint32_t i = 0; i < length; ++i) obj->element(i) = TaggedValue::null();
    stats_.objects_allocated++;
    stats_.bytes_allocated += bytes;
    return obj;
}

// CEP:WHAT: Refills the TLAB; collects when the young space is exhausted.
// CEP:WHY: docs/gc-icggc.md 8.1 trigger path (Rule 82: never abort).
// CEP:STATUS: complete
// CEP:FAILURE: propagates collect_young's named error; OOM surfaces by name.
// CEP:ASSUMES: min_bytes <= young space (else the error names it).
// CEP:COST: @warm on refill, @cold on collection.
// CEP:EVIDENCE: gc_young_evacuation_promotes_and_updates_roots.
Result<void> Heap::refill_tlab(size_t min_bytes) {
    uint8_t* next = align16(tlab_.end());
    const uint8_t* heap_end = young_end();
    if (next + min_bytes > heap_end) {
        // Young space exhausted: evacuate every live object (full
        // promotion, docs/gc-icggc.md 8.1) and restart the bump from the
        // young base. Idempotent under failure — an OOM leaves the young
        // space consistent and the error surfaces (Rule 82).
        stats_.slow_path_allocations++;
        auto collected = collect_young();
        if (!collected) return std::unexpected(collected.error());
        if (min_bytes > young_bytes_) {
            return support::fail(support::ErrorCode::OutOfMemory,
                                 "allocation exceeds the young space (" +
                                     std::to_string(min_bytes) + " bytes)");
        }
        tlab_.reset(young_base(), std::min(young_bytes_, kYoungChunkBytes));
        stats_.tlab_refills++;
        // Allocation-sampling hook (docs/infrastructure/06-observability.md
        // section 6): the WARM refill path only — the hot bump never sees it.
        infra::alloc_sampler().on_refill(
            std::min<size_t>(young_bytes_, kYoungChunkBytes));
        return support::ok();
    }
    tlab_.reset(next, std::min<size_t>(static_cast<size_t>(heap_end - next),
                                       kYoungChunkBytes));
    stats_.tlab_refills++;
    infra::alloc_sampler().on_refill(
        std::min<size_t>(static_cast<size_t>(heap_end - next),
                         kYoungChunkBytes));
    return support::ok();
}

// ---------------------------------------------------------------------------
// Young collection (docs/gc-icggc.md section 8.1): evacuate every live young
// object into old space, update all roots and old slots, reset the young
// space. One mutator pause. Idempotent: re-running after a failure completes
// the remaining closure (already-forwarded bodies are skipped).
// ---------------------------------------------------------------------------

// Local young-evacuation state, shared by the closure helpers below.
namespace {

struct YoungEvacuation {
    Heap& heap;
    std::vector<HeapObject*> worklist;
    std::vector<HeapObject*> promoted;      // fresh bodies (resolution pass)
    std::vector<TaggedValue*> young_slots;  // root slots with young targets
    std::vector<void**> handle_slots;       // handle entries with young targets
    uint64_t promoted_bytes = 0;

    explicit YoungEvacuation(Heap& h) : heap(h) {
        worklist.reserve(64);  // root discovery lands immediately
    }

    /// Queues a young target for copying. Stale old-space oops resolve
    /// through Brooks immediately (the caller's slot is updated in place).
// CEP:WHAT: Roots a slot: young targets join the copy worklist; stale oops resolve.
// CEP:WHY: the young evacuation's root discovery.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: the slot is part of the precise root set.
// CEP:COST: @warm. One branch per slot.
// CEP:EVIDENCE: the young evacuation tests.
    void note_target(TaggedValue* slot) {
        if (!slot->is_heap_object()) return;
        HeapObject* t = slot->as_heap_object();
        if (heap.is_young(t)) {
            young_slots.push_back(slot);
            worklist.push_back(t);
        } else {
            *slot = TaggedValue::heap_pointer(gc_resolve(t));
        }
    }
};

// True when the young body was forwarded by this evacuation (its brooks
// word no longer points at itself). The weak-handle decision reads it.
inline bool gc_is_forwarded(HeapObject* o) noexcept {
    return o->header.brooks != o;
}

// Copies one young body into old space (full promotion). Returns the fresh
// body, or nullptr when old space is exhausted (the error surfaces above).
// CEP:WHAT: Copies one young body into old space with Brooks forwarding.
// CEP:WHY: docs/gc-icggc.md 8.1 evacuation primitive (full promotion).
// CEP:STATUS: complete
// CEP:FAILURE: returns nullptr on old-space exhaustion (named OOM upstream).
// CEP:ASSUMES: o is unforwarded; bytes = the rounded header size.
// CEP:COST: @warm. memcpy + header fixups; queues the copy's young targets.
// CEP:EVIDENCE: gc_young_evacuation_promotes_and_updates_roots.
HeapObject* copy_young_object(YoungEvacuation& ev, HeapObject* o) {
    const size_t bytes = o->header.size;
    HeapObject* n = ev.heap.promote(bytes);
    if (n == nullptr) return nullptr;
    std::memcpy(n, o, bytes);
    n->header.brooks = n;  // fresh body: self-forwarded
    n->header.mark = 0;    // scratch bits never ride a promotion
    n->header.forwarded = 0;
    n->header.pinned = o->header.pinned;  // pinned state follows the object
    o->header.brooks = n;  // Brooks forwarding: the old body freezes
    o->header.forwarded = 1;
    ev.promoted.push_back(n);
    ev.promoted_bytes += bytes;
    // Queue the fresh body's young targets (the closure continues through
    // the copied graph).
    for_each_tagged_slot(n, ev.heap.double_klass(), [&](TaggedValue* s) {
        if (s->is_heap_object()) {
            HeapObject* t = s->as_heap_object();
            if (ev.heap.is_young(t) && gc_resolve(t) == t) {
                ev.worklist.push_back(t);
            }
        }
    });
    return n;
}

}  // namespace

// CEP:WHAT: Young evacuation: promote every live young object, update all roots.
// CEP:WHY: docs/gc-icggc.md 8.1 — one mutator pause, idempotent under failure.
// CEP:STATUS: complete
// CEP:FAILURE: named InvalidState when an old cycle is live; named OutOfMemory when old space exhausts.
// CEP:ASSUMES: the driver never overlaps cycles; the young chain is contiguous.
// CEP:COST: @cold-ish. Bounded by the live young set + root count.
// CEP:EVIDENCE: gc_young_evacuation_promotes_and_updates_roots, the JIT parity test.
Result<void> Heap::collect_young() {
    // The old cycle owns the young space's forwarding protocol: interleaving
    // a young evacuation inside one would break the snapshot (docs/
    // gc-icggc.md 8.8 — the driver never overlaps cycles; enforced here).
    if (engine_->phase() != IcggcEngine::Phase::Idle) {
        return support::fail(support::ErrorCode::InvalidState,
                             "young collection during an old-generation cycle "
                             "(the driver must not overlap cycles)");
    }

    // Adopt the JIT's bump BEFORE the walk: the stencils bump the context
    // copy between slow paths — the young chain below must see the true
    // top or it misses live JIT-allocated objects (the stress suite caught
    // this: the TLAB view lagged and every JIT cell aliased).
    sync_tlab_from_jit();

    PauseTimer pause(engine_->stats_mut(), PauseKind::YoungGc,
                     PauseCause::AllocationSlowPath);
    GcStats& gc_stats = engine_->stats_mut();
    gc_stats.young_collections++;

    RootSet roots;
    scan_runtime_roots(gc_state_, roots, &gc_stats);

    YoungEvacuation ev(*this);
    ev.worklist.reserve(256);

    // 0. Weak handles (Rule 85, 14b-A M1): a weak entry never keeps its
    // target alive. Young targets join the closure ONLY as copying seeds —
    // if they are otherwise unreachable they still get copied (the entry
    // follows), which over-retains by design: the alternative (clearing
    // here) cannot distinguish "died" from "would be reachable via the
    // strong roots". After the closure, unreached entries clear and
    // reached ones resolve (step 7).
    std::vector<void**>& weak_slots = roots.weak_slots;
    for (void** s : weak_slots) {
        auto* t = static_cast<HeapObject*>(*s);
        if (t != nullptr && is_young(t)) ev.worklist.push_back(t);
    }

    // 1. Precise stack slots (T0 registers + JIT frame slots via GC maps).
    for (TaggedValue* s : roots.tagged_slots) ev.note_target(s);
    // 2. Handle entries (raw pointers, updated in place).
    ev.handle_slots = std::move(roots.raw_slots);
    for (void** s : ev.handle_slots) {
        auto* t = static_cast<HeapObject*>(*s);
        if (t == nullptr) continue;
        if (is_young(t)) {
            ev.worklist.push_back(t);
        } else {
            *s = gc_resolve(t);
        }
    }
    // 3. Pinned objects are roots; pinned young objects promote with the
    // pin entry updated in place (identity follows the object, Rule 84).
    for (void* pr : gc_state_.pin_regions) {
        auto* region = static_cast<infra::PinningRegion*>(pr);
        for (void*& p : region->pinned_mut()) {
            auto* t = static_cast<HeapObject*>(p);
            if (t == nullptr) continue;
            if (is_young(t)) {
                ev.worklist.push_back(t);
                // The entry updates after the closure (below).
            } else {
                p = gc_resolve(t);
            }
        }
    }
    // 4. Old -> young edges: dirty old-space cards, walked precisely by
    // region object chain (docs/gc-icggc.md 8.1 — no conservative leg).
    {
        const size_t first_card = (old_base() - young_base()) >> kCardShift;
        const size_t card_count = old_bytes_ >> kCardShift;
        uint32_t last_region = UINT32_MAX;
        for (size_t c = 0; c < card_count; ++c) {
            if (card_table_.card_at(first_card + c) != kCardDirty) continue;
            const uint8_t* card_addr = old_base() + c * kCardBytes;
            Region* r = region_of(card_addr);
            if (r == nullptr || r->index == last_region) continue;
            last_region = r->index;
            // Walk the region's object chain; every tagged slot with a
            // young target is a root edge (recorded for the update pass).
            HeapObject* o = reinterpret_cast<HeapObject*>(r->start);
            uint8_t* rtop = r->top != nullptr
                                ? r->top
                                : r->start + Region::kSize;
            while (o != nullptr &&
                   reinterpret_cast<uint8_t*>(o) < rtop) {
                for_each_tagged_slot(o, double_klass(), [&](TaggedValue* s) {
                    if (s->is_heap_object() && is_young(s->as_heap_object())) {
                        ev.young_slots.push_back(s);
                        ev.worklist.push_back(s->as_heap_object());
                    }
                });
                o = Region::next_object(o, rtop);
            }
        }
    }

    // 5. Copy closure.
    while (!ev.worklist.empty()) {
        HeapObject* o = ev.worklist.back();
        ev.worklist.pop_back();
        if (gc_resolve(o) != o) continue;  // already copied
        if (copy_young_object(ev, o) == nullptr) {
            return support::fail(support::ErrorCode::OutOfMemory,
                                 "young evacuation exhausted old space");
        }
    }

    // 6. Resolution pass: every promoted body's tagged slots resolve now —
    // the closure guarantees every reachable young body is forwarded. These
    // writes are old-space slot stores: they ride the full barrier so the
    // target regions' remembered sets see them (docs/gc-icggc.md 8.3).
    for (HeapObject* n : ev.promoted) {
        for_each_tagged_slot(n, double_klass(), [&](TaggedValue* s) {
            if (s->is_heap_object()) {
                barriered_store(n, s,
                                TaggedValue::heap_pointer(
                                    gc_resolve(s->as_heap_object())));
            }
        });
    }

    // 7. Root slot updates: the recorded slots' targets were reachable, so
    // they are forwarded by now — resolution yields the promoted body.
    gc_stats.promoted_objects +=
        static_cast<uint64_t>(ev.promoted.size());
    gc_stats.promoted_bytes += ev.promoted_bytes;
    for (TaggedValue* s : ev.young_slots) {
        if (s->is_heap_object()) {
            *s = TaggedValue::heap_pointer(gc_resolve(s->as_heap_object()));
        }
    }
    for (void** s : ev.handle_slots) {
        auto* t = static_cast<HeapObject*>(*s);
        if (t != nullptr) *s = gc_resolve(t);
    }
    // Weak handles: reached targets resolve (the entry follows); unreached
    // young targets died — the entry clears (Rule 85's valid-state rule;
    // the young space is about to be reused). Old targets resolved above
    // the closure stay as-is (old bodies do not move during young GC).
    for (void** s : weak_slots) {
        auto* t = static_cast<HeapObject*>(*s);
        if (t == nullptr) continue;
        if (is_young(t)) {
            *s = gc_is_forwarded(t) ? static_cast<void*>(gc_resolve(t))
                                    : nullptr;
        }
    }
    for (void* pr : gc_state_.pin_regions) {
        auto* region = static_cast<infra::PinningRegion*>(pr);
        for (void*& p : region->pinned_mut()) {
            auto* t = static_cast<HeapObject*>(p);
            if (t == nullptr || !is_young(t)) continue;
            // Pinned young targets were closure seeds: reached ones resolve,
            // unreached ones cannot happen (the pin keeps them alive), but
            // the defensive clear mirrors the weak-handle rule.
            p = gc_is_forwarded(t) ? static_cast<void*>(gc_resolve(t))
                                   : nullptr;
        }
    }

    // 8. Card cleanup: the young space resets wholesale (its cards die with
    // it); the processed old-space cards' edges became old->old or died.
    card_table_.clear_all();

    // 9. Reset the young space. Its memory is reused from the base; nothing
    // references it (every live object was promoted and every reference
    // updated through the Brooks protocol). The bound JIT context rewinds
    // to the new window — it must never resume into recycled addresses.
    tlab_.reset(young_base(), std::min(young_bytes_, kYoungChunkBytes));
    publish_tlab_to_jit();
    stats_.tlab_refills++;
    return support::ok();
}

// ---------------------------------------------------------------------------
// Old-cycle region surgery (docs/gc-icggc.md section 8.4). The engine
// orchestrates; these primitives own the bookkeeping.
// ---------------------------------------------------------------------------

// CEP:WHAT: Sweep/reset tail: Free state, epoch bump, remset clear, cards clean.
// CEP:WHY: docs/gc-icggc.md 8.4 — stale remembered-set entries prune by epoch.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: the region's objects are dead or evacuated.
// CEP:COST: @cold. O(cards per region) for the card cleanup.
// CEP:EVIDENCE: the sweep/evacuation tests.
void Heap::reset_region_to_free(Region& r) {
    r.state = RegionState::Free;
    ++r.epoch;  // remembered-set entries logged under the old epoch prune
    r.remset.clear();
    r.live_bytes = 0;
    r.top = r.start;
    free_regions_.push_back(r.index);
    if (bump_region_ == r.index) bump_region_ = UINT32_MAX;
    // The region's cards die with its objects (its old->young edges are gone).
    const size_t base_card = card_table_.index_of(r.start);
    for (size_t c = 0; c < Region::kSize / kCardBytes; ++c) {
        card_table_.set_card(base_card + c, kCardClean);
    }
}

// CEP:WHAT: Evacuates one CSet region: copy+forward, resolve, remset+young updates, reset.
// CEP:WHY: docs/gc-icggc.md 8.4 — region-granularity atomicity between slices.
// CEP:STATUS: complete
// CEP:FAILURE: named OutOfMemory when promotion exhausts old space.
// CEP:ASSUMES: r's state is CSet; marking is sealed (FinalMark ran).
// CEP:COST: @cold-ish. Bounded by the region's live bytes.
// CEP:EVIDENCE: gc_evacuation_moves_objects_and_updates_cross_region_slots.
Result<void> Heap::evacuate_region(Region& r) {
    // Phase 1 — copy marked bodies in chain order, verbatim fields, Brooks
    // forwarding on every frozen body (docs/gc-icggc.md 8.4).
    std::vector<HeapObject*> copied;
    const uint8_t* end = r.top != nullptr ? r.top : r.start + Region::kSize;
    HeapObject* o = reinterpret_cast<HeapObject*>(r.start);
    while (reinterpret_cast<uint8_t*>(o) < end) {
        if (o->header.mark) {
            HeapObject* n = promote(o->header.size);
            if (n == nullptr) {
                // Revert the region (14b-A M2): back to Occupied with the
                // chain's marks cleared so the next cycle's dedup rescans
                // every body; the copied prefix is forwarded and stays
                // reachable through brooks until this region re-evacuates.
                r.state = RegionState::Occupied;
                HeapObject* w = reinterpret_cast<HeapObject*>(r.start);
                while (w != nullptr &&
                       reinterpret_cast<uint8_t*>(w) < end) {
                    w->header.mark = 0;
                    w = Region::next_object(w, r.start + Region::kSize);
                }
                r.live_bytes = 0;
                return support::fail(support::ErrorCode::OutOfMemory,
                                     "evacuation exhausted old space");
            }
            std::memcpy(n, o, o->header.size);
            n->header.brooks = n;
            n->header.mark = 0;
            n->header.forwarded = 0;
            n->header.pinned = 0;
            o->header.brooks = n;  // the frozen body forwards
            o->header.forwarded = 1;
            copied.push_back(n);
            engine_->stats_mut().evacuated_objects++;
            engine_->stats_mut().evacuated_bytes += o->header.size;
        }
        o = Region::next_object(o, r.start + Region::kSize);
        if (o == nullptr) break;
    }
    // Phase 2 — resolve the copies' fields through the full barrier: the
    // writes are old-space slot stores and must log remembered-set entries
    // for their target regions (docs/gc-icggc.md 8.3).
    for (HeapObject* n : copied) {
        for_each_tagged_slot(n, double_klass(), [&](TaggedValue* s) {
            if (s->is_heap_object()) {
                barriered_store(n, s,
                                TaggedValue::heap_pointer(
                                    gc_resolve(s->as_heap_object())));
            }
        });
    }
    // Phase 3 — the region's remembered set: slots in OTHER old regions
    // referencing this one. Owners relocate through Brooks; stale entries
    // (slot no longer references into `r`) skip by the region check.
    for (const RemsetEntry& e : r.remset) {
        const Region* owner_region = region_of(e.owner);
        if (owner_region == nullptr || owner_region->epoch != e.owner_epoch) {
            engine_->stats_mut().remset_pruned++;
            continue;
        }
        HeapObject* owner = gc_resolve(e.owner);
        TaggedValue* slot = e.slot;
        if (owner != e.owner) {
            slot = reinterpret_cast<TaggedValue*>(
                reinterpret_cast<uint8_t*>(owner) +
                (reinterpret_cast<uint8_t*>(e.slot) -
                 reinterpret_cast<uint8_t*>(e.owner)));
        }
        if (!slot->is_heap_object()) continue;
        HeapObject* t = slot->as_heap_object();
        if (region_of(t) != &r) continue;  // stale: overwritten since logged
        barriered_store(owner, slot,
                        TaggedValue::heap_pointer(gc_resolve(t)));
    }
    // Phase 4 — young slots into `r`: the young space is walked as roots
    // every cycle; its referencing slots update here (they are not in any
    // remembered set — docs/gc-icggc.md 8.3).
    for_each_young_object([&](HeapObject* y) {
        for_each_tagged_slot(y, double_klass(), [&](TaggedValue* s) {
            if (!s->is_heap_object()) return;
            HeapObject* t = s->as_heap_object();
            if (region_of(t) != &r) return;
            barriered_store(y, s, TaggedValue::heap_pointer(gc_resolve(t)));
        });
    });
    // Phase 5 — reset. The frozen bodies die here; nothing references them
    // (every referencing slot was updated through the Brooks protocol).
    reset_region_to_free(r);
    return support::ok();
}

// CEP:WHAT: Reclaims a fully-dead region to the free list (Sweep).
// CEP:WHY: docs/gc-icggc.md 8.4 sweep leg.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: the region's live_bytes is 0 (no pinned objects inside).
// CEP:COST: @cold. Constant + card cleanup.
// CEP:EVIDENCE: gc_old_cycle_reclaims_dead_region.
void Heap::reclaim_region(Region& r) {
    engine_->stats_mut().regions_freed++;
    engine_->stats_mut().bytes_reclaimed += Region::kSize;
    reset_region_to_free(r);
}

// CEP:WHAT: Reclaims a dead humongous run (head + continuation regions).
// CEP:WHY: humongous reclamation is the only way they free (they never evacuate).
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: head is a live_bytes == 0 HumongousStart.
// CEP:COST: @cold. Linear over the run.
// CEP:EVIDENCE: gc_humongous_never_moves_and_reclaims_when_dead.
void Heap::reclaim_humongous_run(Region& head) {
    // The run: the head plus contiguous HumongousCont regions.
    engine_->stats_mut().humongous_reclaimed++;
    reset_region_to_free(head);
    for (size_t i = head.index + 1; i < regions_.size(); ++i) {
        Region& cont = regions_[i];
        if (cont.state != RegionState::HumongousCont) break;
        reset_region_to_free(cont);
    }
}

// CEP:WHAT: Clears the mark bits of a surviving region's chain (cycle teardown).
// CEP:WHY: the next InitialMark must start from zero.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: the chain is walkable (bump-allocated).
// CEP:COST: @cold. O(region objects).
// CEP:EVIDENCE: every old-cycle test (two-cycle shapes).
void Heap::clear_region_marks(Region& r) {
    const uint8_t* end = r.top != nullptr ? r.top : r.start + Region::kSize;
    HeapObject* o = reinterpret_cast<HeapObject*>(r.start);
    while (reinterpret_cast<uint8_t*>(o) < end) {
        o->header.mark = 0;
        o = Region::next_object(o, r.start + Region::kSize);
        if (o == nullptr) break;
    }
}

}  // namespace vortex::gc
