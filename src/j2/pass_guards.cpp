// Stages 10/12/13: guard optimization (redundant guard removal and
// strength reductions over the guard kinds).
// Split from src/j2/passes.cpp — one pass per file.
#include "vortex/j2/passes.hpp"

#include <algorithm>
#include <cstring>


#include "passes_internal.hpp"
#include "vortex/runtime/object_model.hpp"

namespace vortex::j2 {
using ir::AccessKind;
using Graph = ir::Graph;
using ir::CallShape;
using ir::CondCode;
using ir::GuardKind;
using ir::JType;
using ir::kNoNode;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;

// ---- stages 10/12/13: guard optimization ----------------------------------------------

uint32_t optimize_guards(ir::Graph& g, BuiltGraph& built) {
    // Redundant guard removal: same guard kind + same checked value + same
    // block + EARLIER IN PROGRAM ORDER -> the later guard's proven value is
    // the earlier one's. Cross-block guard dependence lands with J3.
    // Origin discipline: id order is program order only WITHIN one origin
    // (builder-created vs inlined splice) — after a splice, callee nodes
    // carry HIGH ids but execute at the call's position, so a caller guard
    // may never prove a spliced one (the spliced guard can fail before the
    // caller's pc even runs; its own deopt state is the only exact one).
    uint32_t removed = 0;
    for (uint32_t id = 0; id < g.node_count(); ++id) {
        Node& node = g.node(id);
        if (node.dead || !is_guard_kind(node.kind)) continue;
        if (node.data_inputs.empty()) continue;
        const NodeId value = node.data_inputs[0];
        const bool node_spliced =
            node.id < built.spliced.size() && built.spliced[node.id] != 0;
        for (uint32_t eid = 0; eid < id; ++eid) {
            Node& earlier = g.node(eid);
            if (earlier.dead || earlier.id >= node.id) continue;
            if (earlier.kind != node.kind || earlier.aux != node.aux) {
                continue;
            }
            if (earlier.data_inputs.empty() ||
                earlier.data_inputs[0] != value) {
                continue;
            }
            if (built.block_of[earlier.id] != built.block_of[node.id]) {
                continue;
            }
            const bool earlier_spliced =
                earlier.id < built.spliced.size() &&
                built.spliced[earlier.id] != 0;
            if (earlier_spliced != node_spliced) continue;
            replace_uses(g, node.id, earlier.id);
            node.dead = true;
            ++removed;
            break;
        }
    }
    return removed;
}

}  // namespace vortex::j2
