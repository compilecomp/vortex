// CEP:FILE: include/vortex/j4/max_jit.hpp
// CEP:WHAT: J4 max deterministic optimizing JIT - public engine API (compile_j4, FixedPointState, J4Stats).
// CEP:WHY: The peak tier must enforce Rule 15 (no artificial budget) and Rule 16 (no search) structurally, so the API carries no budget knobs and the termination rule is the graph fingerprint.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: compile_j4 returns named Result failures (build refusal, invalidation); nothing on this surface raises, unwinds, or aborts.
// CEP:ASSUMES: callers pass a verified UGBModule; the persistent store (if given) is used from one mutator at a time (tier-j4.md 12.3 stepwise-driver model).
// CEP:COST: compile-time only. The fixed point costs (pipeline_runs x pipeline cost); on real bodies it converges in 2 runs because every stage is monotone-or-neutral (Rule 55). No runtime execution cost - this header ships no executable code.
// CEP:EVIDENCE: tests/test_j4.cpp - determinism (bit-identical bytes), termination invariants, persistent reuse, worker interleaving, tiering gate; docs/tier-j4.md section 12.
#pragma once

#include <cstdint>

#include "vortex/j2/fast_jit.hpp"
#include "vortex/j3/passes.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::j4 {

using j2::J2Code;
using j2::J2Job;

/// CEP:WHAT: The fixed-point fingerprint: graph hash + live node count.
/// CEP:WHY: The termination rule (spec 12.2) needs a pure, collision-resistant identity of the compiled graph - never of time, address or iteration order.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure struct).
/// CEP:ASSUMES: graph_hash is the canonical j3::graph_hash walk so the escape-summary Identity law and this fingerprint agree.
/// CEP:COST: 16 bytes; filled by one O(live nodes) walk per fixed-point iteration (compile-time cold path, tens of iterations max).
/// CEP:EVIDENCE: j4_fixed_point_termination_invariants (tests/test_j4.cpp).
struct FixedPointState {
    uint64_t graph_hash = 0;
    size_t node_count = 0;
    uint32_t iterations = 0;  ///< pipeline runs after the first
    bool stable = false;      ///< fingerprint unchanged by a full run
    bool cycle = false;       ///< repeated hash: deterministic stop edge
};

/// CEP:WHAT: Aggregate compile telemetry (Rule 120/127).
/// CEP:WHY: Structured, assertable telemetry lets tests lock pipeline behavior without parsing logs.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure struct).
/// CEP:ASSUMES: last_pipeline is a by-value copy - the engine's local would dangle (Rule 69: no lying pointers).
/// CEP:COST: ~1 KB copied once per compile (cold path); no per-instruction cost.
/// CEP:EVIDENCE: j4_fixed_point_termination_invariants asserts pipeline_runs and the stability edge.
struct J4Stats {
    FixedPointState fixed_point;
    uint32_t pipeline_runs = 0;  ///< total J3-driver runs (>= 2: initial +
                                 ///< the confirming run)
    bool reused_ir = false;      ///< the persistent store served the graph
    j3::J3Stats last_pipeline{}; ///< telemetry of the final pipeline run
};

/// CEP:WHAT: Compiles one method through the J4 deterministic engine.
/// CEP:WHY: The M4 deliverable - persistent-IR reuse, the fixed point over the shared optimizer core (Rule 19), and backend emission under the J4 tier identity.
/// CEP:STATUS: complete
/// CEP:FAILURE: Named Result failure on unknown method id or build refusal; the method stays on J3/J2/J1/T0 (Rules 11/76). Never silent.
/// CEP:ASSUMES: job.module is verified and alive through the call; store may be null (compile from bytecode only).
/// CEP:COST: @cold. Build + pipeline runs (2+ on real bodies) + one backend emit. No budget parameter exists by design (Rule 15); memory exhaustion is the graceful-degradation path.
/// CEP:EVIDENCE: tests/test_j4.cpp - bit-identical determinism, persistent-IR reuse parity, worker-vs-synchronous equivalence.
support::Result<J2Code> compile_j4(const J2Job& job,
                                   class PersistentIrStore* store = nullptr,
                                   J4Stats* stats = nullptr);

/// CEP:WHAT: The fingerprint used by the pipeline's termination rule.
/// CEP:WHY: Termination invariant 1 (spec 12.2): a pure function of the live graph, shared construction with the escape-summary Identity hash.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure function).
/// CEP:ASSUMES: the graph's live-node id order is stable within one compile (Rule 48: index edges, dense store).
/// CEP:COST: @cold. One O(live nodes) FNV walk per call; called once per fixed-point iteration.
/// CEP:EVIDENCE: j4_fixed_point_termination_invariants (summary hash == final fingerprint hash).
FixedPointState fingerprint(const ir::Graph& graph);

}  // namespace vortex::j4
