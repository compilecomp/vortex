// LDPT — Lazy-Devirtualized Patch Trampolines (docs/ldpt.md).
//
// The mechanism that bridges static whole-program devirtualization (massive
// compile-time cost) and adaptive IC dispatch (per-call indirect-branch
// overhead). J4 emits a fixed-size Skeleton Trampoline for a speculative
// cross-language call site (UGB CALL_VIRTUAL / CALL_INTERFACE where the
// target type is probable but not guaranteed); the first miss atomically
// hot-patches the exact marshalling instructions at runtime.
//
// Trampoline shape (x86-64, one carve per site):
//
//   [ 0] mov  rax, imm64 primary_klass     ; imm64 patch site
//   [10] cmp  rax, [rsi]                   ; receiver header.klass
//   [13] jne  rel32 -> .hole               ; always-taken placeholder
//   [19] mov  rdi, rsi                     ; receiver -> target ABI
//   [22] call rel32 primary_target         ; placeholder, patched at emit
//   [27] ret                               ; primary path returns to caller
//   [32] .hole (16 bytes, kPatchHoleBytes):
//          mov rdi, imm64 site_handle      ; resolver ABI: rdi = handle
//          jmp rel32 -> resolver thunk     ; tail-jmp: one return address
//          <padding nops>
//   [48] out-of-line reservation (kOolReservationBytes): escalation stubs
//
// Calling convention (docs/ldpt.md section 1): the guest call site passes
// the RAW receiver pointer in rsi; rdi is scratch (the hole loads the site
// handle into it). Escalation stubs tail-jmp into their target, so exactly
// one return address — the original caller's — is ever on the stack.
//
// Escalation state machine (mirrors ugb::IcState, docs/tier-t0.md section 3):
//
//   Resolver -> Mono   : hole jumps to an OOL mono stub (klass guard +
//                        tail-jmp target); further misses rewrite the OOL
//                        stub in place (the hole never changes until Mega).
//   Mono     -> Poly   : OOL stub becomes a kMaxPolyTypes-entry compare
//                        chain (kMaxPolyTypes == ugb::kIcPolyCapacity == 4).
//   Poly     -> Mega   : hole becomes `jmp rel32 mega_thunk`; the mega
//                        dispatch reads a plain-RW data row published by
//                        atomic pointer swizzling — updates need NO safe
//                        point and NO session (docs/ldpt.md section 3).
//
// Patching protocol (docs/ldpt.md section 2): codegen into scratch, M:N
// safe-point freeze via the HandshakeManager, one PatchArena session (W^X
// arm), patch, resume. The resolver is @cold (bounded per (site, type)
// pair); steady-state execution never re-enters C++.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "vortex/infra/dependency.hpp"
#include "vortex/infra/patch_arena.hpp"
#include "vortex/infra/threading.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::runtime::ldpt {

// ---- semantic-domain constants (CEM-26 section 2) ---------------------------

/// Reserved hole size inside every skeleton. Two worst-case hole shapes must
/// fit: `mov rdi, imm64` (10) + `jmp rel32` (5) + 1 padding nop.
inline constexpr size_t kPatchHoleBytes = 16;
/// Fixed prologue bytes before the hole (see the file header layout).
inline constexpr size_t kTrampolinePrologueBytes = 48;
/// Out-of-line reservation per site: a poly-4 compare chain (4*28 + 15 tail)
/// and the mega thunk both must fit without re-carving (arena layout is
/// frozen at publish).
inline constexpr size_t kOolReservationBytes = 256;
inline constexpr size_t kTrampolineBytes =
    kTrampolinePrologueBytes + kPatchHoleBytes + kOolReservationBytes;
/// Megamorphic dispatch row capacity. Overflow beyond this is a QoS event
/// (rows are rebuilt per escalation; capacity bounds the scan).
inline constexpr uint32_t kMegaRowCapacity = 16;

/// Escalation states, mirroring the T0 IC state machine. Kept a distinct
/// type: the IC tracks field/call feedback; the trampoline tracks NATIVE
/// patch state. Same shape, different domains.
enum class TrampolineState : uint8_t { Resolver, Mono, Poly, Mega };

/// One polymorphic entry of the compare chain (klass pointer -> target).
struct PolyTypeEntry {
    uint64_t klass = 0;
    uint64_t target = 0;
};

/// C++-side metadata for one patched call site. The machine code lives in
/// the arena; this record carries the patch-state and escalation history.
struct TrampolineSite {
    uint64_t site_id = 0;         // ugb::make_site_id (Rule 8: stable)
    uint32_t handle = 0;          // hole-load imm32; index into the site table
    size_t trampoline_offset = 0; // trampoline base in the arena
    size_t hole_offset = 0;       // 16-byte hole in the arena
    size_t ool_offset = 0;        // out-of-line reservation base
    TrampolineState state = TrampolineState::Resolver;
    uint64_t primary_klass = 0;
    uint64_t primary_target = 0;
    uint8_t poly_count = 0;
    PolyTypeEntry poly[ugb::kIcPolyCapacity]{};
    uint32_t method_id = 0;
    uint32_t instruction_index = 0;

    /// The dispatch entry for `klass` in the CURRENT state, or nullptr.
    /// @hot — C++ mirror of the machine-code compare chain; used by tests
    /// and by future C++-side dispatch fallbacks.
    // PERF_CONTRACT:
    // BUDGET: <= 5 cycles steady state (primary test, one poly scan step on
    //         a 4-entry array held in one cache line)
    // READS: 64 bytes (the site record — poly entries are line-local)
    // WRITES: 0
    // BRANCHES: primary hit: 1; poly: <= 4 compares, linear, predicted on
    //           the hot entry
    // CACHE: one line; the site record is the whole working set
    const PolyTypeEntry* lookup(uint64_t klass) const noexcept;
};

/// Megamorphic dispatch row: plain RW data, NEVER executable. The mega
/// thunk loads `active` and scans entries — updates swizzle `active` with a
/// release store and require no safe point (docs/ldpt.md section 3).
struct MegaRow {
    uint32_t count = 0;
    struct Entry {
        uint64_t klass = 0;
        uint64_t target = 0;
    };
    Entry entries[kMegaRowCapacity]{};

    /// @hot — C++ mirror of the mega thunk scan (same contract as
    /// TrampolineSite::lookup, scan bounded by kMegaRowCapacity).
    const Entry* lookup(uint64_t klass) const noexcept;
};

/// Per-site megamorphic dispatch state. Retired rows are kept alive because
/// a concurrent reader may hold a stale pointer until its next acquire load.
struct MegaDispatch {
    std::atomic<MegaRow*> active{nullptr};
    std::vector<std::unique_ptr<MegaRow>> retired;

    /// Publishes old+entry as a fresh row and swizzles the active pointer.
    /// @cold — one call per NEW megamorphic type per site.
    support::Result<const MegaRow::Entry*> publish_entry(uint64_t klass,
                                                         uint64_t target);
};

/// LDPT configuration knobs (CEM-26 section 2: named, documented).
struct LdptConfig {
    size_t arena_bytes = 64 * 1024;
};

/// The LDPT manager: owns the patch arena, the site table, the resolver
/// thunk, and the escalation protocol.
class LdptManager {
public:
    explicit LdptManager(LdptConfig config = {},
                         infra::HandshakeManager* handshake = nullptr,
                         infra::DependencyGraph* deps = nullptr);

    /// Emits a skeleton trampoline for one speculative call site and
    /// registers the compile-time assumption with the dependency graph.
    /// The primary (probable) type's marshalling is live immediately; the
    /// hole routes every other type to the resolver.
    support::Result<TrampolineSite*> emit_skeleton(
        uint32_t method_id, uint32_t instruction_index, uint64_t primary_klass,
        uint64_t primary_target);

    /// The resolver (tail-called from every hole/miss path). rdi = manager
    /// (via the thunk), esi = site handle, rdx = raw receiver. Escalates the
    /// site for the receiver's klass, patches the arena under the safe-point
    /// protocol, and tail-jmps to the resolved target — the receiver never
    /// sees an IC lookup on the steady-state path.
    /// PERF_PERMIT PERF-007: this path is reached once per (site, type)
    /// pair; the whole cost (codegen + page flip) is amortized to zero by
    /// the patched steady state (docs/ldpt.md section 5 contract table).
    static uint64_t resolve_miss(LdptManager* manager, uint32_t handle,
                                 void* receiver) noexcept;

    /// Dependency-driven invalidation (docs/ldpt.md section 4A): reverts the
    /// hole to the resolver stub so the site degrades safely instead of
    /// dangling. Returns the site, or nullptr when the id is unknown.
    support::Result<TrampolineSite*> invalidate(uint64_t site_id);

    /// Applies an invalidation batch produced by the DependencyGraph.
    support::Result<void> invalidate_batch(
        const infra::InvalidationBatch& batch);

    /// Runtime target resolution hook (docs/ldpt.md section 2 step 2). In
    /// production this calls J4 dispatch tables / J1-J2 stencil codegen; M1
    /// wires a host-provided function. Returning ok=false aborts patching
    /// and yields 0 from the resolver (guest-visible as a clean miss).
    using TargetResolver = uint64_t (*)(void* user, uint32_t method_id,
                                        uint32_t instruction_index,
                                        void* receiver, bool* ok) noexcept;
    void set_target_resolver(TargetResolver fn, void* user) noexcept {
        target_resolver_ = fn;
        resolver_user_ = user;
    }

    const TrampolineSite* site(uint32_t handle) const noexcept {
        return handle < sites_.size() ? &sites_[handle] : nullptr;
    }
    const TrampolineSite* find_site(uint64_t site_id) const noexcept;
    size_t site_count() const noexcept { return sites_.size(); }
    const infra::PatchArena& arena() const noexcept { return arena_; }
    infra::PatchArena& arena_mutable() noexcept { return arena_; }

    /// Test/observability view of a site's active megamorphic row.
    const MegaRow* mega_row_for(uint32_t handle) const noexcept {
        return handle < mega_rows_.size()
                   ? mega_rows_[handle].active.load(std::memory_order_acquire)
                   : nullptr;
    }

    LdptManager(const LdptManager&) = delete;
    LdptManager& operator=(const LdptManager&) = delete;
    // Non-movable by contract: the emitted resolver thunk embeds `this` as
    // an imm64 and each mega thunk embeds &mega_rows_[h].active — moving a
    // manager would dangle every emitted code sequence.
    LdptManager(LdptManager&&) = delete;

    /// Executable entry of a site's trampoline (for tests and J4 wiring).
    void* entry_of(const TrampolineSite& site) const noexcept {
        return const_cast<uint8_t*>(arena_.at(site.trampoline_offset));
    }

    /// Assembles `bytes` at `offset` during the EMISSION phase.
    support::Result<void> emit_bytes(std::span<const uint8_t> bytes,
                                     size_t offset) {
        return arena_.write(bytes, offset);
    }

    /// Publishes the arena (RW -> RX) after emission. Sites emitted later
    /// would violate W^X; the manager is emit-once by design.
    support::Result<void> publish() { return arena_.publish(); }

    /// Stats for the observability sink (docs/infrastructure/06).
    struct Stats {
        uint64_t skeletons_emitted = 0;
        uint64_t misses_resolved = 0;
        uint64_t mono_patches = 0;
        uint64_t poly_patches = 0;
        uint64_t mega_escalations = 0;
        uint64_t invalidations = 0;
    };
    const Stats& stats() const noexcept { return stats_; }

    /// Target ABI for patched marshalling: raw receiver -> result bits.
    using TargetFn = uint64_t (*)(void* receiver) noexcept;

private:
    support::Result<void> escalate(TrampolineSite& site, uint64_t klass,
                                   uint64_t target);
    support::Result<void> patch_session(
        const std::vector<std::pair<size_t, std::span<const uint8_t>>>& writes);
    support::Result<size_t> assemble_ool_stub(
        const TrampolineSite& site, uint8_t* out, size_t cap,
        size_t* out_size) const;
    void emit_resolver_thunk();
    std::vector<uint8_t> assemble_skeleton(const TrampolineSite& site) const;
    TrampolineSite* find_site_mutable(uint64_t site_id) noexcept;

    infra::PatchArena arena_;
    infra::HandshakeManager* handshake_ = nullptr;  // M:N protocol (M2 binds)
    infra::DependencyGraph* deps_ = nullptr;
    LdptConfig config_;
    std::vector<TrampolineSite> sites_;
    // Per-site megamorphic rows, parallel to sites_ (indexed by handle).
    // ONE SHARED ROW WOULD CROSS DISPATCH TARGETS BETWEEN SITES: site B's
    // publish would find site A's (klass -> target) entry and dispatch A's
    // target. The deque never relocates existing elements (MegaDispatch is
    // non-movable: it holds an atomic), and the mega thunk embeds
    // &mega_rows_[handle].active.
    std::deque<MegaDispatch> mega_rows_;
    size_t thunk_offset_ = 0;     // per-arena resolver thunk
    TargetResolver target_resolver_ = nullptr;
    void* resolver_user_ = nullptr;
    Stats stats_;
};

// ---- layout invariants (CEM-26 section 9) -----------------------------------

static_assert(kPatchHoleBytes >= 16,
              "the worst-case hole (mov rdi, imm64 + jmp rel32 + nop) needs "
              "16 bytes; smaller holes cannot express the resolver ABI");
static_assert(kOolReservationBytes >= 256,
              "a poly-4 compare chain plus the mega thunk must fit the "
              "out-of-line reservation: the arena layout is frozen at "
              "publish and stubs rewrite in place");
static_assert(sizeof(TrampolineSite) <= 192,
              "the site record is the escalation working set; the 4 poly "
              "entries (64B) dominate and stay within 3 cache lines");

}  // namespace vortex::runtime::ldpt
