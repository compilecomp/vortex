// CEP:FILE: src/j4/engine.hpp
// CEP:WHAT: The internal J4 engine Session: the fixed-point compile split into capture/advance/emit/store transitions.
// CEP:WHY: One loop implementation shared by the synchronous engine and the stepwise worker - the two must never drift in termination behavior (spec 12.2/12.3).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: capture returns named build refusals; the other transitions fail closed per the contracts on their definitions in max_jit.cpp.
// CEP:ASSUMES: internal to src/j4 - not installed, not part of the public API.
// CEP:COST: @cold. The Session carries one graph state + a bounded seen-hash flat map; per-transition costs are stated on the definitions.
// CEP:EVIDENCE: every test in tests/test_j4.cpp exercises the Session through both drivers.
//
// Transition map:
//   capture()  - build or reuse the graph state (spec 12.1)
//   advance()  - ONE pipeline run + termination evaluation (spec 12.2)
//   emit()     - the shared backend emit (Rule 19)
//   store_result() - return the graph state to the persistent store
#pragma once

#include <memory>

#include "vortex/j2/fast_jit.hpp"
#include "vortex/j3/passes.hpp"
#include "vortex/j4/max_jit.hpp"
#include "vortex/j4/persistent_ir.hpp"
#include "vortex/support/containers.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::j4::detail {

/// Why the last advance() ended.
enum class Advance : uint8_t {
    Ran,      // the graph moved; keep iterating
    Stable,   // fingerprint unchanged: fixed point reached
    Cycle,    // repeated hash: deterministic stop edge
};

class Session {
public:
    /// Builds or reuses the graph state (spec 12.1). Named refusals only.
    support::Result<void> capture(const j2::J2Job& job,
                                  PersistentIrStore* store);

    /// Runs exactly one pipeline iteration and evaluates the termination
    /// rule. Must not be called before a successful capture().
    Advance advance();

    /// Emits through the shared backend under Tier::J4.
    support::Result<J2Code> emit(const j2::J2Job& job);

    /// Hands the pipeline-processed state back to the store (if any).
    /// `state` records the lifecycle stage: the synchronous engine stores
    /// Draft; the worker stores Published after its publication step
    /// (spec 12.1's states are observable, not decorative).
    void store_result(PersistentIrStore* store, const j2::J2Job& job,
                      PersistentIrEntry::State state =
                          PersistentIrEntry::State::Draft);

    bool captured() const noexcept { return built_ != nullptr; }
    bool done() const noexcept { return done_; }
    const FixedPointState& state() const noexcept { return fp_; }
    const J4Stats& stats() const noexcept { return stats_; }

private:
    std::unique_ptr<j2::BuiltGraph> built_;
    j3::J3Budget budget_;
    j3::J3Stats pipeline_stats_;
    ir::EscapeSummary summary_;
    support::FlatHashMap<uint64_t, uint8_t> seen_;
    FixedPointState fp_;
    J4Stats stats_;
    uint64_t version_hash_ = 0;
    bool started_ = false;
    bool done_ = false;
    Advance last_advance_ = Advance::Ran;
};

}  // namespace vortex::j4::detail
