// Stage 36: bounds-check elimination, driven by the stage-10 ranges.
// Split from src/j3/passes_j3.cpp — one pass per file.
#include "passes_internal.hpp"


namespace vortex::j3 {

using namespace vortex::j2;
using ir::Graph;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;
using BuiltGraph = j2::BuiltGraph;

// ---- stage 36: bounds-check elimination (range-driven) -----------------------
uint32_t eliminate_bounds_checks(Graph& g, BuiltGraph& built,
                                 const std::vector<Range>& ranges) {
    uint32_t killed = 0;
    for (const NodeId gid : built.guards) {
        Node& n = g.node(gid);
        if (n.dead || n.kind != NodeKind::BoundsGuard) continue;
        if (n.data_inputs.size() < 2) continue;
        const Range& idx = ranges[n.data_inputs[0]];
        const Range& len = ranges[n.data_inputs[1]];
        // Proven in-bounds: index within [0, len.lo) with len provably at
        // least idx.hi + 1. With the M0 ISA this fires on constant-length
        // or pre-proven index paths; the loop-IV correlation arrives with
        // stage 37 (tracked, docs/tier-j3.md).
        if (idx.lo >= 0 && idx.hi < len.lo) {
            n.dead = true;
            ++killed;
        }
    }
    if (killed != 0) {
        g.eliminate_dead_nodes();
        j2::sync_built(g, built);
    }
    return killed;
}

}  // namespace vortex::j3
