// Stages 30 + 31: natural-loop identification + loop-invariant code
// motion (guards stay put — their FrameState is pc-exact, Rule 30).
// Split from src/j3/passes_j3.cpp — one pass per file.
#include "passes_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace vortex::j3 {

using namespace vortex::j2;
using ir::Graph;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;
using BuiltGraph = j2::BuiltGraph;

// ---- stages 30 + 31: loop identification + LICM -------------------------------
//
// Natural loops over the block CFG (backedge = edge to a dominator). A pure
// node all of whose inputs are defined outside the loop and whose block is
// inside the loop hoists to the preheader (the loop header's idom block).
// Guards do NOT hoist: their FrameState is exact for their own pc (Rule 30/39)
// and re-deriving it at the target point is J4 machinery.
struct LoopSet {
    // loops[i] = {header, body blocks}; deterministic RPO discovery order.
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> loops;
};

LoopSet identify_loops(const BuiltGraph& built,
                       const std::vector<uint32_t>& idom) {
    LoopSet out;
    auto dominates = [&](uint32_t a, uint32_t b) {
        while (b != UINT32_MAX) {
            if (b == a) return true;
            if (b == 0) return false;
            b = idom[b];
        }
        return false;
    };
    for (const uint32_t b : built.rpo) {
        for (const uint32_t pred : built.blocks[b].preds) {
            if (!dominates(b, pred)) continue;  // not a backedge
            // Natural loop of b via reverse reachable set from pred.
            std::vector<uint32_t> body{b};
            std::vector<uint8_t> in(built.blocks.size(), 0);
            in[b] = 1;
            std::vector<uint32_t> stack{pred};
            in[pred] = 1;
            while (!stack.empty()) {
                const uint32_t x = stack.back();
                stack.pop_back();
                body.push_back(x);
                for (const uint32_t p : built.blocks[x].preds) {
                    if (!in[p]) {
                        in[p] = 1;
                        stack.push_back(p);
                    }
                }
            }
            out.loops.push_back({b, body});
            break;  // one loop per header per pass run (deterministic)
        }
    }
    return out;
}

uint32_t licm(Graph& g, BuiltGraph& built) {
    std::vector<uint32_t> rpo;
    std::vector<uint32_t> idom;
    j2::compute_dominators(built, rpo, idom);
    const LoopSet loops = identify_loops(built, idom);
    uint32_t hoisted = 0;
    for (const auto& [header, body] : loops.loops) {
        // Preheader = the header's immediate dominator (deterministic choice;
        // a dedicated preheader block split is J4 loop machinery).
        const uint32_t preheader = idom[header];
        if (preheader == header || preheader == UINT32_MAX) continue;
        for (const uint32_t b : body) {
            for (uint32_t id = 0; id < g.node_count(); ++id) {
                const Node& n = g.node(id);
                if (n.dead || built.block_of[id] != b) continue;
                if (!ir::is_pure(n.kind) || n.kind == NodeKind::Parameter) {
                    continue;
                }
                // Div/Rem trap on their operands (Rule 110): hoisting moves
                // the trap out of its loop domain — T0 would not trap for
                // a zero-iteration loop. Not infallible -> stays put.
                if (n.kind == NodeKind::Div || n.kind == NodeKind::Rem) {
                    continue;
                }
                if (n.kind == NodeKind::Phi) continue;  // loop phis stay
                bool inputs_outside = true;
                for (const NodeId in : n.data_inputs) {
                    if (in == ir::kNoNode) continue;
                    const Node& inp = g.node(in);
                    if (inp.dead) continue;
                    const uint32_t ib = built.block_of[in];
                    const bool inside =
                        std::find(body.begin(), body.end(), ib) != body.end();
                    if (inside && in != id) inputs_outside = false;
                }
                if (!inputs_outside) continue;
                // Hoist: reblock to the preheader.
                built.block_of[id] = preheader;
                ++hoisted;
            }
        }
    }
    if (hoisted != 0) j2::sync_built(g, built);
    return hoisted;
}

}  // namespace vortex::j3
