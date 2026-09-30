// J3 — the budgeted full pass pipeline (docs/tier-j3.md section 2, the 60
// named stages in three waves; docs/ir-son-ciog.md section 3 stage map).
//
// Honest-stage contract (Rule 76 applied to telemetry): every one of the 60
// named stages runs through this driver exactly once per compile. A stage
// either (a) transforms the graph and reports its transformation count,
// (b) delegates to an already-implemented stage (the J2 scalar core or the
// shared backend), or (c) reports NotApplicable with a NAMED domain reason
// — a stage whose guest-ISA domain is absent at the current UGB level
// never silently pretends to run, and never fabricates transformations.
//
// Laws honored here:
//   R54 — every stage declares its contract (the table below IS the
//         declaration; budget behavior is per-stage).
//   R55 — every transforming stage is monotone in node count.
//   R56 — deterministic: fixed stage order, id-order node walks, min-id
//         tie-breaks, no hash iteration in decisions.
//   R59/131 — per-stage kill switches; a killed stage is a no-op.
//   R120/127 — telemetry is structured and assertable.
#pragma once

#include <cstdint>
#include <vector>

#include "vortex/ir/escape_summary.hpp"
#include "vortex/j2/graph_builder.hpp"
#include "vortex/j2/passes.hpp"
#include "vortex/support/result.hpp"

namespace vortex::j3 {

/// Per-stage outcome (Rule 120/127: structured, assertable telemetry).
enum class StageStatus : uint8_t {
    Ran,            // transformed (count may still be 0 on this input)
    Delegated,      // executed by the J2 pipeline / shared backend stage
    NotApplicable,  // domain absent at the current UGB level (named reason)
    Killed,         // kill switch off (Rule 59/131)
    BudgetStop,     // node budget reached before this stage
};

struct StageResult {
    StageStatus status = StageStatus::Ran;
    uint32_t transformations = 0;
    const char* reason = nullptr;  // NotApplicable / Killed: why
};

/// J3 budgets (docs/tier-j3.md section 1). Reaching a limit still emits
/// good code: the driver stops optimizing and the backend emits the
/// current graph.
struct J3Budget {
    size_t node_cap = 200'000;
    uint32_t inline_depth_cap = 4;
    uint32_t inline_site_cap = 32;
    size_t inline_callee_node_cap = 512;
    uint64_t pass_control = ~0ull;  // bit i = stage i (1-based) disabled
};

/// Pipeline telemetry: the 60 stage slots + aggregate counters
/// (docs/tier-j3.md section 2 numbering).
struct J3Stats {
    j2::PipelineStats j2_core;  // the delegated scalar-core counters
    struct Stage {
        const char* name = nullptr;
        StageResult result;
    };
    Stage stages[60];
    uint32_t scalar_replaced = 0;   // allocations removed (stage 16)
    uint32_t loads_forwarded = 0;   // store-to-load forwardings (stage 7)
    uint32_t guards_dominated = 0;  // redundancy kills (stages 41/42/26)
    uint32_t hoisted_loops = 0;     // LICM motions (stage 31)
    uint32_t bounds_killed = 0;     // BCE kills (stage 36)
    uint32_t published_summaries = 0;  // stage 29
};

/// Runs the full J3 pipeline (stages 2-54 graph-level; 1/55-60 live in the
/// builder/shared backend). Deterministic; monotone in node count per
/// transforming stage. Returns the escape summary of the compiled body
/// (stage 29 output) for the caller to publish.
ir::EscapeSummary run_j3_pipeline(ir::Graph& graph, j2::BuiltGraph& built,
                                  const J3Budget& budget, J3Stats& stats);

/// The canonical graph hash (Rule 56): an FNV-1 walk over live nodes in id
/// order (kind, aux, payload, data inputs). ONE construction shared by the
/// escape-summary Identity binding (docs/xlea.md section 4.1) and the J4
/// fixed-point fingerprint (docs/tier-j4.md section 12.2) — the two must
/// never disagree about a graph's identity.
uint64_t graph_hash(const ir::Graph& graph);

}  // namespace vortex::j3
