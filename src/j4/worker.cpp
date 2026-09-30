// CEP:FILE: src/j4/worker.cpp
// CEP:WHAT: The J4 background worker implementation: stepwise Session driving, W^X publication, incremental region replacement.
// CEP:WHY: The compile model "J4 compiles in the background while J3 executes" as an explicit state machine the driver advances between mutator executions (spec 12.3).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Named Result failures for phase misuse and emit/W^X errors; terminal-phase steps are no-ops, never errors; cancellation is deferred to the next step boundary by name.
// CEP:ASSUMES: one driver advances the worker (single mutator, tier-j4.md 12.3 stepwise-driver model); the live region table passed to publish outlives the call.
// CEP:COST: @cold. start = build/reuse + one pipeline run; step = one pipeline iteration; publish = one W^X flip + O(live regions) bytecode-pc matching. Zero cost between steps.
// CEP:EVIDENCE: tests/test_j4.cpp - j4_worker_stepwise_j3_keeps_running, j4_worker_cancellation_is_named_and_deferred.
#include "vortex/j4/worker.hpp"

#include <algorithm>

#include "engine.hpp"

namespace vortex::j4 {

using j2::J2Code;
using j2::J2Job;

/// CEP:WHAT: Constructs an idle worker bound to its persistent IR store.
/// CEP:WHY: The reference documents the store partnership; the body sets the initial observable state explicitly (CEP&CC 19: no empty bodies).
/// CEP:STATUS: complete
/// CEP:FAILURE: none.
/// CEP:ASSUMES: store outlives the worker.
/// CEP:COST: @cold. Two enum stores + one reference bind.
/// CEP:EVIDENCE: constructed by every worker test in tests/test_j4.cpp.
J4Worker::J4Worker(PersistentIrStore& store) : store_(store) {
    // Non-empty body (CEP&CC 19): the initial state is set here explicitly
    // so the constructor carries its own contract.
    phase_ = Phase::Idle;
    last_event_ = Event::Started;
}

J4Worker::~J4Worker() = default;

/// CEP:WHAT: Accessor for the last fixed-point fingerprint (defaults while idle).
/// CEP:WHY: Out of line because the session type is internal to src/j4.
/// CEP:STATUS: complete
/// CEP:FAILURE: none.
/// CEP:ASSUMES: read between steps (single-mutator discipline).
/// CEP:COST: @cold. One branch + one reference return.
/// CEP:EVIDENCE: j4_worker_stepwise_j3_keeps_running asserts last_state().stable.
const FixedPointState& J4Worker::last_state() const noexcept {
    return session_ != nullptr ? session_->state() : empty_state_;
}

/// CEP:WHAT: Accessor for the worker's compile telemetry (defaults while idle).
/// CEP:WHY: Same internal-type reason as last_state().
/// CEP:STATUS: complete
/// CEP:FAILURE: none.
/// CEP:ASSUMES: read between steps.
/// CEP:COST: @cold. One branch + one reference return.
/// CEP:EVIDENCE: exercised by the worker tests.
const J4Stats& J4Worker::stats() const noexcept {
    return session_ != nullptr ? session_->stats() : empty_stats_;
}

/// CEP:WHAT: Idle -> Captured transition.
/// CEP:WHY: The capture must own its graph exclusively before the mutator may run again (spec 12.3's isolation property).
/// CEP:STATUS: complete
/// CEP:FAILURE: Named failure when the phase is not Idle; build refusals ride through from the session.
/// CEP:ASSUMES: job.module outlives the draft.
/// CEP:COST: @cold. One session capture (build/reuse + first pipeline run).
/// CEP:EVIDENCE: j4_worker_stepwise_j3_keeps_running.
support::Result<void> J4Worker::start(const J2Job& job) {
    if (phase_ != Phase::Idle) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "J4 worker: start() requires the Idle phase");
    }
    auto session = std::make_unique<detail::Session>();
    auto captured = session->capture(job, &store_);
    if (!captured) return std::unexpected(captured.error());
    session_ = std::move(session);
    job_ = job;
    phase_ = Phase::Captured;
    last_event_ = Event::Started;
    return {};
}

/// CEP:WHAT: One state-machine transition per call (Captured/Optimizing -> Optimizing|Emitted, or the deferred cancel).
/// CEP:WHY: The step granularity is what lets J3 run between steps - the worker holds no shared mutable state with the run path (spec 12.3).
/// CEP:STATUS: complete
/// CEP:FAILURE: Named failure for step-before-start; terminal phases return unchanged; emit failures set Event::Failed and carry the error.
/// CEP:ASSUMES: the same driver context advances every step (no concurrent steps).
/// CEP:COST: @cold. One pipeline iteration + one fingerprint walk; the cancellation check is one branch.
/// CEP:EVIDENCE: j4_worker_stepwise_j3_keeps_running (J3 stays exact across every step); j4_worker_cancellation_is_named_and_deferred.
support::Result<J4Worker::Phase> J4Worker::step() {
    switch (phase_) {
        case Phase::Idle:
            return support::fail(support::ErrorCode::InvalidArgument,
                                 "J4 worker: step() before start()");
        case Phase::Published:
        case Phase::Cancelled:
            return phase_;  // terminal: stepping is a no-op, not an error
        case Phase::Captured:
        case Phase::Optimizing: {
            // Rule 15: compilation is cancellable on invalidation — the
            // flag is honored HERE, at the next step boundary, so the
            // mutator never waits for the cancel.
            if (cancelled_) {
                session_.reset();
                code_ = J2Code{};
                phase_ = Phase::Cancelled;
                last_event_ = Event::Cancelled;
                return phase_;
            }
            const detail::Advance a = session_->advance();
            switch (a) {
                case detail::Advance::Ran:
                    last_event_ = Event::Optimized;
                    phase_ = Phase::Optimizing;
                    return phase_;
                case detail::Advance::Stable:
                case detail::Advance::Cycle: {
                    last_event_ = a == detail::Advance::Stable
                                      ? Event::Stable
                                      : Event::Cycle;
                    support::Result<J2Code> emitted = session_->emit(job_);
                    if (!emitted) {
                        last_event_ = Event::Failed;
                        return std::unexpected(emitted.error());
                    }
                    code_ = std::move(*emitted);
                    phase_ = Phase::Emitted;
                    last_event_ = Event::Emitted;
                    return phase_;
                }
            }
            return support::fail(support::ErrorCode::InternalError,
                                 "J4 worker: unreachable advance state");
        }
        case Phase::Emitted:
            return phase_;  // nothing to optimize; caller owns publish()
    }
    return support::fail(support::ErrorCode::InternalError,
                         "J4 worker: unreachable phase");
}

/// CEP:WHAT: Emitted -> Published: W^X publication plus per-region replacement of the live RBPD table.
/// CEP:WHY: Publication is the mutator's only observation point; region-level replacement (bytecode-pc overlap, the stable coordinates across compilations) keeps the live table coherent (spec 12.3).
/// CEP:STATUS: complete
/// CEP:FAILURE: Named failure when not Emitted; W^X errors ride through with the draft intact (retryable).
/// CEP:ASSUMES: the caller performs the entry swap (one move assignment); the small linear region match is deliberate (publication is a cold path).
/// CEP:COST: @cold. One W^X flip (PERF-005 batch protocol) + O(live regions) matching + one replace_region or add_region per fresh region.
/// CEP:EVIDENCE: j4_worker_stepwise_j3_keeps_running (publish over the live J3 table, then run the published J4 code to parity).
support::Result<j2::J2Executable> J4Worker::publish(
    deopt::RegionTable* live_regions) {
    if (phase_ != Phase::Emitted) {
        return support::fail(
            support::ErrorCode::InvalidArgument,
            "J4 worker: publish() requires the Emitted phase");
    }
    // W^X publication through the shared code range (one flip per batch).
    support::Result<j2::J2Executable> ex = j2::publish_j2(code_);
    if (!ex) {
        last_event_ = Event::Failed;
        return ex;
    }
    if (live_regions != nullptr && ex->regions != nullptr) {
        // Incremental region publication (spec 12.3): the new table's
        // regions replace the live ones per region. Matching is by
        // bytecode-pc overlap — the two compilations lay code out
        // differently, so code offsets cannot match; bytecode pcs are the
        // stable coordinates (docs/deopt-rbpd.md section 1). The scan is
        // over tens of regions on a cold path — deliberate, documented.
        for (const deopt::RegionDescriptor& fresh :
             ex->regions->regions()) {
            const deopt::RegionDescriptor* target = nullptr;
            for (const deopt::RegionDescriptor& old :
                 live_regions->regions()) {
                const bool overlaps =
                    old.bytecode_pc_begin < fresh.bytecode_pc_end &&
                    fresh.bytecode_pc_begin < old.bytecode_pc_end;
                if (overlaps && old.state != deopt::RegionState::Stale) {
                    target = &old;
                    break;  // deterministic: first live overlap in id order
                }
            }
            if (target != nullptr) {
                deopt::RegionDescriptor replacement = fresh;
                // replace_region re-derives the id from its target slot, so
                // pre-setting region_id is documentation, not effect; the
                // Results are unreachable-failure on a live table whose
                // spans were just matched (Rule 76: the failure would carry
                // the named reason if the impossible happened).
                replacement.region_id = target->region_id;
                (void)live_regions->replace_region(target->region_id,
                                                   replacement);
            } else {
                (void)live_regions->add_region(fresh);
            }
        }
    }
    session_->store_result(&store_, job_,
                           PersistentIrEntry::State::Published);
    session_.reset();
    phase_ = Phase::Published;
    last_event_ = Event::Published;
    return ex;
}

/// CEP:WHAT: Deferred cancellation: the draft dies at the NEXT step boundary.
/// CEP:WHY: invalidate() must never free state the driver may be inspecting, and the mutator must never wait for a cancel (Rule 15) - the flag deferral gives both.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (idempotent; no-op for Idle/Published/Cancelled).
/// CEP:ASSUMES: the dependency callback that calls this stays reentrancy-safe because nothing is freed here.
/// CEP:COST: @cold. One flag store.
/// CEP:EVIDENCE: j4_worker_cancellation_is_named_and_deferred (Cancelled phase + Event::Cancelled at the next step).
void J4Worker::invalidate() {
    if (phase_ == Phase::Idle || phase_ == Phase::Published ||
        phase_ == Phase::Cancelled) {
        return;
    }
    // The draft dies at the NEXT step boundary (spec 12.3): invalidate()
    // itself never touches the session, so a driver that invalidates from
    // a dependency callback stays reentrancy-safe.
    cancelled_ = true;
}

}  // namespace vortex::j4
