// CEP:FILE: include/vortex/j4/worker.hpp
// CEP:WHAT: The J4 background worker: stepwise compile state machine with incremental region publication (spec 12.3).
// CEP:WHY: "J4 compiles in the background while J3 executes" without the M6 threading infra - the driver advances the worker between mutator executions, and the mutator only observes publication points.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: start/step/publish return named Result failures for phase misuse and emit errors; invalidate() is an exception-free flag honored at the next step boundary.
// CEP:ASSUMES: one worker per compile draft (one-shot lifecycle); the live region table handed to publish() outlives the call.
// CEP:COST: @cold. Per step: one fixed-point iteration (compile-time). Publication: one W^X flip + a linear region match over tens of regions. Zero runtime cost between steps - the worker holds no interpreter state and takes no locks.
// CEP:EVIDENCE: j4_worker_stepwise_j3_keeps_running (J3 parity across steps), j4_worker_cancellation_is_named_and_deferred (tests/test_j4.cpp).
#pragma once

#include <cstdint>
#include <memory>

#include "vortex/deopt/rbpd.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/j4/max_jit.hpp"
#include "vortex/j4/persistent_ir.hpp"
#include "vortex/support/result.hpp"

namespace vortex::j4 {

namespace detail {
class Session;  // src/j4/engine.hpp — the shared fixed-point loop
}

class J4Worker {
public:
    enum class Phase : uint8_t {
        Idle = 0,
        Captured,
        Optimizing,
        Emitted,
        Published,
        Cancelled,
    };

    /// CEP:WHAT: Named events for the observability sink (Rule 120).
    /// CEP:WHY: Tier transitions and compile progress must be spellable, never guessed (Rules 28/76).
    /// CEP:STATUS: complete
    /// CEP:FAILURE: none (pure enum).
    /// CEP:ASSUMES: consumers map every enumerator to a stable string at the observability boundary.
    /// CEP:COST: 1 byte per value; stored once on the worker.
    /// CEP:EVIDENCE: j4_worker_cancellation_is_named_and_deferred asserts Event::Cancelled.
    enum class Event : uint8_t {
        Started = 0,
        Optimized,    // one advance ran; the graph moved
        Stable,       // fixed point reached
        Cycle,        // repeated-hash stop edge
        Emitted,      // backend emit completed
        Published,
        Cancelled,
        Failed,       // emit failed; the error rides the Result
    };

    /// CEP:WHAT: Constructs an idle worker bound to a persistent IR store.
    /// CEP:WHY: The worker moves graph states through the store; the reference documents that partnership (Rule 69).
    /// CEP:STATUS: complete
    /// CEP:FAILURE: none.
    /// CEP:ASSUMES: store outlives the worker.
    /// CEP:COST: @cold. One reference store.
    /// CEP:EVIDENCE: constructed by every worker test in tests/test_j4.cpp.
    explicit J4Worker(PersistentIrStore& store);  // out of line (internal
                                                  // session member)
    ~J4Worker();
    J4Worker(const J4Worker&) = delete;
    J4Worker& operator=(const J4Worker&) = delete;
    J4Worker(J4Worker&&) = delete;
    J4Worker& operator=(J4Worker&&) = delete;

    /// CEP:WHAT: Idle -> Captured: takes the persistent-IR snapshot (the session moves the graph state out of the store; the store gets it back at publication).
    /// CEP:WHY: The capture must own its graph exclusively so the mutator's J3 execution never shares mutable state with the compile (spec 12.3).
    /// CEP:STATUS: complete
    /// CEP:FAILURE: Named failure when not Idle, or the build refusal rides through from the session (Rule 76).
    /// CEP:ASSUMES: job.module stays alive until publish/invalidate completes.
    /// CEP:COST: @cold. One build (or store reuse) + the first pipeline run - identical cost model to compile_j4's capture phase.
    /// CEP:EVIDENCE: j4_worker_stepwise_j3_keeps_running (start then interleaved steps).
    support::Result<void> start(const j2::J2Job& job);

    /// CEP:WHAT: Advances exactly one transition (spec 12.3).
    /// CEP:WHY: Stepwise progress is what lets J3 keep running: between any two steps the mutator may execute freely (the worker holds no shared mutable state with the run path).
    /// CEP:STATUS: complete
    /// CEP:FAILURE: Named failure for step-before-start; terminal phases return unchanged; an emit failure surfaces as Failed with the error.
    /// CEP:ASSUMES: the driver calls step() from the same thread/context that owns the engine (single mutator, tier-j4.md 12.3 stepwise-driver model).
    /// CEP:COST: @cold. One pipeline iteration + one fingerprint walk per call; cancellation check is one branch.
    /// CEP:EVIDENCE: j4_worker_stepwise_j3_keeps_running (J3 exact across every step); j4_worker_cancellation_is_named_and_deferred (Cancel honored at the next step).
    support::Result<Phase> step();

    /// CEP:WHAT: Emitted -> Published: W^X publication of the emitted code, then incremental region replacement into `live_regions` when provided.
    /// CEP:WHY: Publication is the mutator's only observation point: per-region replacement (bytecode-pc overlap matching) keeps the live RBPD table coherent while the fresh code publishes (spec 12.3).
    /// CEP:STATUS: complete
    /// CEP:FAILURE: Named failure when not Emitted, or the W^X error rides through; the draft stays intact on failure.
    /// CEP:ASSUMES: the caller performs the entry-point swap (one move into its executable slot); the region table spans are bytecode-pc matched (code offsets differ between compilations). Quiescence note: between the region replacement and the caller's entry swap, the live table's fresh regions carry the NEW code's offsets while old code may still execute — the driver must treat (replacement + swap) as one quiescent step (single-mutator, tier-j4.md 12.3: no guest code runs inside a driver step).
    /// CEP:COST: @cold. One W^X flip (PERF-005 batch protocol) + a linear match over tens of regions + per-region replace_region calls.
    /// CEP:EVIDENCE: j4_worker_stepwise_j3_keeps_running (publish with the live J3 table, executed J4 artifact to parity, post-publication region shape asserted).
    support::Result<j2::J2Executable> publish(
        deopt::RegionTable* live_regions = nullptr);

    /// CEP:WHAT: External invalidation (Rule 15: cancellable). The draft dies at the NEXT step boundary, by name.
    /// CEP:WHY: Cancellation must never wait on the compile and must never free state the driver is mid-inspecting - deferring to the step boundary keeps both properties.
    /// CEP:STATUS: complete
    /// CEP:FAILURE: none (idempotent; no-op for terminal phases).
    /// CEP:ASSUMES: reentrancy safety comes from the flag deferral (a dependency callback may call this while the driver holds the worker).
    /// CEP:COST: @cold. One flag store; the drop happens at the next step().
    /// CEP:EVIDENCE: j4_worker_cancellation_is_named_and_deferred (Cancelled phase + named event).
    void invalidate();

    Phase phase() const noexcept { return phase_; }
    Event last_event() const noexcept { return last_event_; }
    /// CEP:WHAT: Out-of-line accessors for the last fingerprint and telemetry.
    /// CEP:WHY: The session type is internal (src/j4/engine.hpp), so these cannot be inline without leaking the include.
    /// CEP:STATUS: complete
    /// CEP:FAILURE: none (return the stored defaults when idle).
    /// CEP:ASSUMES: called from the owning driver between steps.
    /// CEP:COST: @cold. One branch + one reference return per call.
    /// CEP:EVIDENCE: j4_worker_stepwise_j3_keeps_running asserts last_state().stable.
    const FixedPointState& last_state() const noexcept;
    const J4Stats& stats() const noexcept;

private:
    PersistentIrStore& store_;
    std::unique_ptr<detail::Session> session_;
    j2::J2Job job_;
    J2Code code_;
    Phase phase_ = Phase::Idle;
    Event last_event_ = Event::Started;
    bool cancelled_ = false;  // honored at the next step() (Rule 15)
    FixedPointState empty_state_{};
    J4Stats empty_stats_{};
};

}  // namespace vortex::j4
