// Stage 2: canonicalization (commutand normalization, idempotent
// rewrites). Also defines clone_const — the shared constant-
// materialization helper used by folding and inlining.
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

// ---- stage 2: canonicalization -----------------------------------------------------

NodeId clone_const(ir::Graph& g, BuiltGraph& built, NodeId near_id,
                   int64_t value, JType t) {
    const NodeId c = g.add_const(value);
    sync_built(g, built);
    built.block_of[c] = built.block_of[near_id];
    built.types[c] = t;
    return c;
}

uint32_t canonicalize(ir::Graph& g, BuiltGraph& built) {
    uint32_t rewrites = 0;
    for (uint32_t id = 0; id < g.node_count(); ++id) {
        Node& node = g.node(id);
        if (node.dead) continue;
        const NodeKind k = node.kind;
        if (!is_pure_kind(k) || node.data_inputs.empty()) continue;
        const NodeId a = node.data_inputs[0];

        if (node.data_inputs.size() == 2) {
            const NodeId b = node.data_inputs[1];
            const Node& bn = g.node(b);
            const bool b_zero_smi = bn.kind == NodeKind::Const &&
                                    bn.const_value == 0;
            const bool b_one_smi = bn.kind == NodeKind::Const &&
                                   bn.const_value == 2;  // smi 1 = 1<<1
            switch (k) {
            case NodeKind::Add:
            case NodeKind::Sub:
            case NodeKind::Or:
            case NodeKind::Xor:
                if (b_zero_smi) {
                    replace_uses(g, node.id, a);
                    kill(g, node.id);
                    ++rewrites;
                    continue;
                }
                break;
            case NodeKind::Mul:
                if (b_zero_smi) {
                    // x * 0 == 0 exactly (no overflow path).
                    const NodeId zero = clone_const(g, built, node.id, 0,
                                                    JType::Smi);
                    replace_uses(g, node.id, zero);
                    kill(g, node.id);
                    ++rewrites;
                    continue;
                }
                if (b_one_smi) {
                    replace_uses(g, node.id, a);
                    kill(g, node.id);
                    ++rewrites;
                    continue;
                }
                break;
            default:
                break;
            }
            // x ^ x -> 0 (exact for every tagged word).
            if (k == NodeKind::Xor && a == b) {
                const NodeId zero = clone_const(g, built, node.id, 0,
                                                JType::Smi);
                replace_uses(g, node.id, zero);
                kill(g, node.id);
                ++rewrites;
                continue;
            }
            // x - x -> 0 (exact: no overflow when both are the same value).
            if (k == NodeKind::Sub && a == b) {
                const NodeId zero = clone_const(g, built, node.id, 0,
                                                JType::Smi);
                replace_uses(g, node.id, zero);
                kill(g, node.id);
                ++rewrites;
                continue;
            }
        }
        // Boxing round-trips (see the builder's Untag/Tag domain):
        // Tag(Untag(v)) -> v and Untag(Tag(v)) -> v.
        if (k == NodeKind::Tag || k == NodeKind::Untag) {
            const Node& in = g.node(node.data_inputs[0]);
            if (in.kind == (k == NodeKind::Tag ? NodeKind::Untag
                                               : NodeKind::Tag)) {
                replace_uses(g, node.id, in.data_inputs[0]);
                kill(g, node.id);
                ++rewrites;
                continue;
            }
        }
        // Trivial Phis (single distinct input after CFG folding).
        if (k == NodeKind::Phi) {
            NodeId only = kNoNode;
            bool trivial = true;
            for (const NodeId in : node.data_inputs) {
                if (in == kNoNode) {
                    trivial = false;
                    break;
                }
                if (only == kNoNode) {
                    only = in;
                } else if (only != in) {
                    trivial = false;
                    break;
                }
            }
            if (trivial && only != kNoNode && only != node.id) {
                replace_uses(g, node.id, only);
                kill(g, node.id);
                ++rewrites;
                continue;
            }
        }
    }
    return rewrites;
}

}  // namespace vortex::j2
