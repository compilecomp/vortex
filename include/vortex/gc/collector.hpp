// CEP:FILE: include/vortex/gc/collector.hpp
// CEP:WHAT: The ICGGC old-generation cycle driver (phase machine, pacing
// config, Rule-120 GC telemetry) and the published-code registry the
// precise stack walker resolves JIT frames through.
// CEP:WHY: docs/gc-icggc.md section 8.4/8.5/8.6 — pause-bounded concurrent
// marking/sweeping/evacuation with Brooks forwarding, and the GC side of
// the JIT metadata interchange (Rule 86).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: step()/maybe_start_cycle()/run_cycle() return named Result
// failures (SatbClosureViolated, OutOfMemory, InvalidState); the engine
// moves to Failed and back to Idle on one-shot observation.
// CEP:ASSUMES: one mutator thread (M0 contract); the driver never overlaps
// an old cycle with a young collection; GcCodeRecord lifetime is owned by
// the published executable's shared_ptr.
// CEP:COST: @cold. Slices are config-bounded (mark items / regions per
// step); the pacer is a pure function of named inputs — no wall clock in
// sizing. Registry lookup is a linear weak-ptr walk with amortized prune.
// CEP:EVIDENCE: tests/test_gc.cpp — gc_engine_phase_machine_steps_through_
// ladder, gc_satb_closure_* (DoD 2), gc_pause_p99_under_budget (DoD 1).
// ICGGC old-generation cycle driver (docs/gc-icggc.md section 8.4).
//
// IcggcEngine advances the phase machine in driver-driven slices; the
// mutator runs between engine calls — the same stepwise-driver concurrency
// contract as the J4 worker (docs/tier-j4.md 12.3). M6 threading moves the
// slices to a background thread without redesign: every phase boundary is
// already a quiescent step, and the SATB/forwarding protocols carry the
// mutator's interleaved stores and reads.
//
// GcCodeRegistry is the GC side of the JIT metadata interchange
// (docs/gc-icggc.md section 7): published JIT code registers its code range,
// its GC maps, and its frame geometry so the precise stack walker
// (src/gc/stack_scan.cpp) can scan every JIT frame at a pause.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "vortex/gc/icggc.hpp"
#include "vortex/support/result.hpp"

namespace vortex::gc {

/// Every pause event the engine attributes to the mutator (docs/gc-icggc.md
/// 8.5: slices and STW phases are both pause events; the DoD p99 covers ALL
/// of them). Named for the observability sink (Rule 120).
enum class PauseKind : uint8_t {
    YoungGc = 0,
    InitialMark,
    FinalMark,
    UpdateRoots,
    MarkSlice,
    EvacuateSlice,
    SweepSlice,
};

/// WHY the pause happened (docs/infrastructure/06-observability.md section 6:
/// p99 debugging starts from "what started it", not just "how long"). The
/// engine carries one cycle-scoped cause; the young GC is always the
/// allocation slow path.
enum class PauseCause : uint8_t {
    Unspecified = 0,
    AllocationSlowPath,
    OccupancyTrigger,
    ExplicitRequest,
};

// CEP:WHAT: Maps a PauseCause to its stable observability name.
// CEP:WHY: Rule 120: causes must be spellable, never guessed.
// CEP:STATUS: complete
// CEP:FAILURE: none (pure enum mapping).
// CEP:ASSUMES: consumers keep the strings stable across releases.
// CEP:COST: @cold. One switch, returns a static literal.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 15 (the cause labels are
//          asserted there; the driver-facing breakdown rides batch e's
//          stats work).
const char* pause_cause_name(PauseCause cause) noexcept;

// CEP:WHAT: Maps a PauseKind to its stable observability name.
// CEP:WHY: Rule 120: pause events must be spellable, never guessed.
// CEP:STATUS: complete
// CEP:FAILURE: none (pure enum mapping).
// CEP:ASSUMES: consumers keep the strings stable across releases.
// CEP:COST: @cold. One switch, returns a static literal.
// CEP:EVIDENCE: used by the pause ring's dump path and tests.
const char* pause_kind_name(PauseKind kind) noexcept;

/// One recorded pause (ns monotonic duration + kind + cause). The ring is
/// bounded (Rule 28: bounded telemetry buffers). The defaulted cause keeps
/// the M5 aggregate initialization and the two-argument record_pause
/// compiling unchanged.
struct PauseEvent {
    uint64_t duration_ns = 0;
    PauseKind kind = PauseKind::YoungGc;
    PauseCause cause = PauseCause::Unspecified;
};

/// Rule-120 telemetry: every counter a driver/test can assert on.
struct GcStats {
    uint64_t young_collections = 0;
    uint64_t promoted_objects = 0;
    uint64_t promoted_bytes = 0;
    uint64_t old_cycles = 0;
    uint64_t satb_logs = 0;
    uint64_t satb_tail_entries = 0;
    uint64_t remset_entries = 0;
    uint64_t remset_pruned = 0;
    uint64_t regions_freed = 0;
    uint64_t bytes_reclaimed = 0;
    uint64_t evacuated_objects = 0;
    uint64_t evacuated_bytes = 0;
    uint64_t humongous_reclaimed = 0;
    uint64_t weak_handles_cleared = 0;
    uint64_t closure_checks = 0;
    uint64_t mark_queue_pops = 0;
    uint64_t jit_frames_scanned = 0;
    uint64_t t0_frames_scanned = 0;
    uint64_t slice_steps = 0;
    /// Bounded pause ring (docs/gc-icggc.md 8.5) + the p99 helper's source.
    std::vector<PauseEvent> pauses;
    static constexpr size_t kMaxPauseEvents = 4096;

    void record_pause(PauseKind kind, uint64_t duration_ns) {
        record_pause(kind, duration_ns, PauseCause::Unspecified);
    }
    void record_pause(PauseKind kind, uint64_t duration_ns, PauseCause cause);
    /// Nearest-rank percentile over the recorded ring (0 <= pct <= 100).
    uint64_t pause_p99_ns() const { return pause_percentile_ns(99); }
    uint64_t pause_percentile_ns(unsigned pct) const;
    uint64_t max_pause_ns() const;
};

// CEP:WHAT: RAII pause accounting: records the scope's ns duration into the stats ring.
// CEP:WHY: docs/gc-icggc.md 8.5 — slices and STW phases are both pause events.
// CEP:STATUS: complete
// CEP:FAILURE: none (the destructor always records).
// CEP:ASSUMES: one PauseTimer live per pause scope; GcStats outlives it.
// CEP:COST: @cold. One steady_clock read at construct + destruct.
// CEP:EVIDENCE: the DoD p99 gate (gc_pause_p99_under_budget) is computed from its records.
class PauseTimer {
public:
    /// The legacy shape (cause = Unspecified) — defined out-of-line so the
    /// delegating body carries its CEP block (CEP&CC 10.5: no empty bodies).
    PauseTimer(GcStats& stats, PauseKind kind);
    PauseTimer(GcStats& stats, PauseKind kind, PauseCause cause);
    ~PauseTimer();
    PauseTimer(const PauseTimer&) = delete;
    PauseTimer& operator=(const PauseTimer&) = delete;

private:
    GcStats& stats_;
    PauseKind kind_;
    PauseCause cause_;
    uint64_t start_ns_;
};

/// The precise root set of one pause (docs/gc-icggc.md 8.6): T0 register
/// slots and JIT frame slots (tagged, updated in place), handle entries
/// (raw, updated in place; weak entries tracked separately — they never
/// keep objects alive, Rule 85), and pinned objects (roots, immobile).
struct RootSet {
    std::vector<TaggedValue*> tagged_slots;
    std::vector<void**> raw_slots;
    std::vector<void**> weak_slots;
    std::vector<HeapObject*> pinned;
};

// CEP:WHAT: Collects the precise root set: T0 frames, JIT frames (GC maps),
// handles, pins.
// CEP:WHY: docs/gc-icggc.md 8.6 — every pause and the UpdateRoots phase
// walk the same entry point.
// CEP:STATUS: complete
// CEP:FAILURE: named failures ride the cycle error path; a null anchor
// simply yields no JIT frames.
// CEP:ASSUMES: GcRuntimeState is wired before any collection; one mutator.
// CEP:COST: @warm. O(frames + slots); the JIT walk is registry-bounded.
// CEP:EVIDENCE: gc_young_gc_inside_jit_allocation_keeps_parity exercises
// the JIT leg end-to-end.
void scan_runtime_roots(const GcRuntimeState& state, RootSet& out,
                        GcStats* stats = nullptr);

/// Walks the mutator state (T0 frames, JIT frames through the GC-map
/// registry, handles, pins) and collects every root slot. Implemented in
/// src/gc/stack_scan.cpp. Clears the JIT frame anchor after consuming it —
/// the anchor is only valid while the storing stencil's frame is live
/// (docs/gc-icggc.md 8.6).
// CEP:WHAT: Collects the precise root set: T0 frames, JIT frames (GC maps), handles, pins.
// CEP:WHY: docs/gc-icggc.md 8.6 — every pause and the UpdateRoots phase walk the same entry.
// CEP:STATUS: complete
// CEP:FAILURE: named failures ride the cycle error path (a null anchor simply yields no JIT frames).
// CEP:ASSUMES: GcRuntimeState is wired before any collection; one mutator.
// CEP:COST: @warm. O(frames + slots); the JIT walk is registry-bounded.
// CEP:EVIDENCE: gc_young_gc_inside_jit_allocation_keeps_parity exercises the JIT leg end-to-end.


/// Slice sizing (docs/gc-icggc.md section 8.5). The pacer is a pure
/// function of named inputs — no wall clock in the sizing path (the DoD
/// measures pauses; it does not steer them), so pacing is deterministic
/// under test.
struct IcggcConfig {
    /// Assertion mode (docs/gc-icggc.md 8.4): FinalMark verifies the SATB
    /// closure over the recorded snapshot. Tests/stress only.
    bool assertion_mode = false;
    /// Mark-slice work unit: mark-queue pops per ConcurrentMark step.
    uint32_t mark_slice_items = 256;
    /// Sweep-slice work unit: region reclaims per Sweep step.
    uint32_t sweep_slice_regions = 4;
    /// Evacuate-slice work unit: CSet regions per Evacuate step (one region
    /// per slice is the contract's atomicity unit; >1 batches small sets).
    uint32_t evacuate_regions_per_slice = 1;
    /// Old-cycle trigger: old-space occupancy percent (promotion targets +
    /// humongous + occupied live bytes over old_bytes).
    uint32_t old_gc_trigger_pct = 75;
    /// CSet selection: a region with live_bytes <= this percent of a full
    /// region is fragmentation worth evacuating.
    uint32_t evacuate_live_ratio_pct = 25;
    /// DoD pause budget (docs/gc-icggc.md 8.9): p99 over all recorded
    /// pauses must stay under it on the allocation-stress suite.
    uint64_t pause_budget_ns = 1'000'000;
};

/// The old-generation cycle (docs/gc-icggc.md section 8.4):
///   Idle -> InitialMark -> ConcurrentMark -> FinalMark -> Evacuate
///        -> UpdateRoots -> Sweep -> Idle
/// Each step() advances ONE phase transition or ONE bounded slice. STW
/// phases (InitialMark, FinalMark, UpdateRoots) complete inside their step;
/// ConcurrentMark/Evacuate/Sweep advance in slices sized by the config.
class IcggcEngine {
public:
    enum class Phase : uint8_t {
        Idle = 0,
        InitialMark,
        ConcurrentMark,
        FinalMark,
        Evacuate,
        UpdateRoots,
        Sweep,
        Failed,  // a named cycle error; Idle after the driver observes it
    };

    explicit IcggcEngine(Heap& heap);
    ~IcggcEngine();
    IcggcEngine(const IcggcEngine&) = delete;
    IcggcEngine& operator=(const IcggcEngine&) = delete;

    IcggcConfig config;

    /// Trigger check (docs/gc-icggc.md 8.4): starts a cycle when old-space
    /// occupancy crosses config.old_gc_trigger_pct. Idempotent while a
    /// cycle is running. Drivers call it between mutator executions. A
    /// cycle IT starts is cause-tagged OccupancyTrigger (the pause ring's
    /// cause label, docs/infrastructure/06 section 6).
    support::Result<void> maybe_start_cycle();

    /// Advances exactly one transition or slice (the stepwise contract).
    /// Idle steps are a named no-op (returns Idle). Errors surface with a
    /// named reason and move the engine to Failed.
    support::Result<Phase> step();

    /// Runs the cycle to completion (driver convenience; the DoD stress
    /// driver steps instead to interleave mutator work). Starts through the
    /// occupancy trigger like any cycle — the pause-ring cause stays the
    /// initiator's (ExplicitRequest is the adaptive-initiation batch's
    /// reserved label).
    support::Result<void> run_cycle();

    Phase phase() const noexcept { return phase_; }
    const GcStats& stats() const noexcept { return stats_; }
    GcStats& stats_mut() noexcept { return stats_; }

    /// Old-space occupancy percent (the trigger's input; tests assert on it).
    uint32_t old_occupancy_pct() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Heap& heap_;
    Phase phase_ = Phase::Idle;
    GcStats stats_;
    /// The pause-ring cause label the running cycle's phases inherit
    /// (docs/infrastructure/06-observability.md section 6): stamped by
    /// maybe_start_cycle (OccupancyTrigger — the ONLY start mechanism
    /// today; run_cycle routes through the trigger too). ExplicitRequest
    /// is the adaptive-initiation batch's reserved label.
    PauseCause cycle_cause_ = PauseCause::Unspecified;
};

/// GC-side JIT metadata record (docs/gc-icggc.md sections 7/8.6). Published
/// executables own one shared record; the registry holds weak references so
/// released code leaves the walk automatically.
struct GcCodeRecord {
    enum class Kind : uint8_t { J1, J2Shape };
    const uint8_t* code_base = nullptr;
    size_t code_bytes = 0;
    Kind kind = Kind::J2Shape;
    /// J1: the method's vreg count (vregs at [rbp + vreg_disp(rc, i)]).
    /// J2Shape: unused — the GC maps' bit count sizes the scan.
    uint32_t register_count = 0;
    std::shared_ptr<const std::vector<uint8_t>> gc_maps;
};

/// The published-code registry (docs/gc-icggc.md 8.6). Weak entries: a
/// released executable's record expires and the walker skips it — no
/// destructor hooks in the executable types beyond dropping their
/// shared_ptr. Single mutator, no locks (M0 contract).
class GcCodeRegistry {
public:
    /// Registers one published executable; returns the record the caller
    /// keeps alive inside its executable type.
    std::shared_ptr<GcCodeRecord> register_code(
        const uint8_t* base, size_t bytes, GcCodeRecord::Kind kind,
        uint32_t register_count,
        std::shared_ptr<const std::vector<uint8_t>> gc_maps);

    /// The record whose code range contains `pc`, or nullptr. Prunes
    /// expired entries as it walks (amortized cleanup).
    const GcCodeRecord* lookup(uintptr_t pc) noexcept;

    size_t live_count() const noexcept;

private:
    std::vector<std::weak_ptr<GcCodeRecord>> entries_;
};

/// Process-wide registry (one runtime per process, M0 contract). Published
/// executables register through this accessor.
GcCodeRegistry& gc_code_registry() noexcept;

}  // namespace vortex::gc
