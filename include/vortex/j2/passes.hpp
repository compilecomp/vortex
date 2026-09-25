// J2 pass pipeline (docs/tier-j2.md section 2 — the 21 named stages; stages
// 1/18-21 live in the builder / regalloc / backend). Every pass declares a
// Rule-54 contract (required/produced properties, invalidated analyses,
// budget behavior, determinism) and is individually kill-switchable
// (Rule 59/131). Passes are idempotent or monotonic (Rule 55): each either
// shrinks the node count or leaves it unchanged.
#pragma once

#include <cstdint>
#include <vector>

#include "vortex/ir/son_graph.hpp"
#include "vortex/j2/graph_builder.hpp"
#include "vortex/support/containers.hpp"
#include "vortex/support/result.hpp"

namespace vortex::j2 {

/// Per-pass kill switches (Rule 59/131). A disabled pass keeps the graph as
/// it arrived — correctness never depends on an optimization.
enum PassControl : uint64_t {
    PassCanonicalize = 1ull << 0,
    PassFoldConstants = 1ull << 1,
    PassSCCP = 1ull << 2,
    PassDCE = 1ull << 3,
    PassValueNumber = 1ull << 4,      // local + scoped-global (stages 7+8)
    PassReduceStrength = 1ull << 5,   // stages 9+11 (range-gated; no-op
                                      // without a range proof)
    PassGuards = 1ull << 6,           // stages 10+12+13 (null/class guard
                                      // strength reduction + redundancy)
    PassInline = 1ull << 7,           // stages 14+15
    PassSpecializeIC = 1ull << 8,     // stage 16
    PassLayout = 1ull << 9,           // stage 17
    PassAll = (1ull << 10) - 1,
};

/// Pipeline budget (docs/tier-j2.md section 1). Exceeding the node cap must
/// degrade gracefully: the driver stops optimization and emits the current
/// graph (never fails the compile).
struct PipelineBudget {
    size_t node_cap = 20'000;
    uint32_t inline_depth_cap = 2;
    uint32_t inline_site_cap = 8;
    /// Straight-line node budget for one inlined callee (stages 14/15).
    size_t inline_callee_node_cap = 64;
    uint64_t pass_control = PassAll;
};

/// Stage summaries for telemetry (Rule 127: structured, stable).
struct PipelineStats {
    uint32_t folded_constants = 0;
    uint32_t value_numbered = 0;
    uint32_t guards_removed = 0;
    uint32_t nodes_dead = 0;
    uint32_t blocks_unreachable = 0;
    uint32_t inlined_calls = 0;
    uint32_t specialized_sites = 0;
    uint32_t budget_stop = 0;  // 0 = completed; else the stage index that
                               // hit the cap
};

/// Runs the graph-level pipeline (stages 2-17) over the built graph.
/// Deterministic (Rule 56); monotone in node count per pass (Rule 55).
PipelineStats run_pipeline(ir::Graph& graph, BuiltGraph& built,
                           const PipelineBudget& budget);

/// The block order the backend emits (stage 17 output; RPO when layout is
/// disabled). Deterministic given the same graph + profiles.
std::vector<uint32_t> block_layout_order(const BuiltGraph& built);

/// Dominance placement: for every node, the block where codegen must define
/// it (the LCA of its use blocks; definitions fall back to their creation
/// block when they have no users). Consumed by the backend.
std::vector<uint32_t> dominance_placement(const ir::Graph& graph,
                                          const BuiltGraph& built);

}  // namespace vortex::j2
