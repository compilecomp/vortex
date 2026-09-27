// Stage 29: interprocedural EA — the escape summary of THIS body.
// Split from src/j3/passes_j3.cpp — one pass per file.
#include "passes_internal.hpp"

#include <functional>
#include "vortex/support/containers.hpp"

namespace vortex::j3 {

using namespace vortex::j2;
using ir::Graph;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;
using BuiltGraph = j2::BuiltGraph;

// ---- stage 29: interprocedural EA — the escape summary of THIS body -----------
ir::EscapeSummary summarize(const Graph& g, const BuiltGraph& built,
                            uint64_t graph_hash) {
    ir::EscapeSummary s;
    s.method_id = built.method_id;
    s.graph_hash = graph_hash;
    // Parameters in id order (the builder emits one Parameter per vreg in
    // the entry block, id order == vreg order).
    std::vector<NodeId> params;
    for (uint32_t id = 0; id < g.node_count(); ++id) {
        if (!g.node(id).dead && g.node(id).kind == NodeKind::Parameter) {
            params.push_back(id);
        }
    }
    s.param_count = static_cast<uint32_t>(params.size());
    s.params.assign(s.param_count, ir::ParamEscape::NoEscape);
    // Memoized escape propagation: phi webs can be cyclic (a,b = b,a swap
    // loops) — the recursion MUST terminate (an in-progress entry resolves
    // the cycle: a phi cycle alone does not escape; real escapes reach the
    // memo through non-phi users).
    support::FlatHashMap<NodeId, uint8_t> memo;  // bit0 set = escaped
    std::function<bool(NodeId)> escapes_value = [&](NodeId v) -> bool {
        if (const uint8_t* m = memo.find(v)) return (*m & 1) != 0;
        memo.insert(v, 0);  // in progress
        bool result = false;
        for (uint32_t id = 0; id < g.node_count() && !result; ++id) {
            const Node& n = g.node(id);
            if (n.dead) continue;
            for (size_t i = 0; i < n.data_inputs.size(); ++i) {
                if (n.data_inputs[i] != v) continue;
                if (n.kind == NodeKind::FrameState) continue;  // remat
                if (n.kind == NodeKind::Return) { result = true; break; }
                if (n.kind == NodeKind::Store && i == 1) { result = true; break; }
                if (n.kind == NodeKind::Call) { result = true; break; }
                if (n.kind == NodeKind::Allocate) { result = true; break; }
                if (n.kind == NodeKind::Phi && n.id != v) {
                    if (escapes_value(n.id)) { result = true; break; }
                }
            }
        }
        memo.insert(v, static_cast<uint8_t>(result ? 3 : 2));  // bit1 = done
        return result;
    };
    for (uint32_t p = 0; p < params.size(); ++p) {
        if (escapes_value(params[p])) s.params[p] = ir::ParamEscape::ArgEscape;
    }
    for (uint32_t id = 0; id < g.node_count(); ++id) {
        const Node& n = g.node(id);
        if (n.dead || n.kind != NodeKind::Return) continue;
        for (const NodeId in : n.data_inputs) {
            if (in != ir::kNoNode && g.node(in).kind == NodeKind::Allocate) {
                s.returns_heap_allocation = true;
            }
        }
    }
    return s;
}

}  // namespace vortex::j3
