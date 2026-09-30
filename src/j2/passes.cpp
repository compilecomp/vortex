// J2 graph-level pass pipeline. See the header for the contract inventory.
//
// Implementation notes the laws force:
//   - Replacement is by index sweep (Rule 48): every use of a node is a u32
//     in some node's data_inputs/control/effect_in, so the replace_* helpers
//     walk the node store once — no use-def lists at J2 scale.
//   - Folding refuses operations whose T0 semantics TRAP on the constant
//     inputs (typed smi overflow, SHL leaving the smi range): folding them
//     would erase an observable error (Rules 107/110). Only exactly
//     representable results fold.
//   - SCCP rewires the CFG when a conditional folds; the dominator tree is
//     recomputed once after the SCCP/DCE pair, not per pass.
//   - Value numbering scopes along the dominator tree; only PURE nodes join,
//     plus same-block redundant loads with no intervening effect.
//   - Inlining (stages 14/15) is bounded and real: direct calls, single-block
//     callees, depth/site/node caps, and deopt frames that chain the caller's
//     FrameState so a guard failure reconstructs BOTH frames (Rule 39/113).
#include "vortex/j2/passes.hpp"

#include <algorithm>
#include <cstring>

#include "vortex/runtime/object_model.hpp"

#include "passes_internal.hpp"
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

// The Smi domain bounds are TaggedValue::smi_min()/smi_max()
// (include/vortex/support/tagged_value.hpp) — ONE spelling for every tier
// (the M4 unification; the old local pair here had a sign bug in the min
// bound, silently disabling Smi-const folding).

// ---- growth helper -----------------------------------------------------------

/// Grows the BuiltGraph side tables to match the node store.
void sync_built(const ir::Graph& g, BuiltGraph& built) {
    const size_t n = g.node_count();
    if (built.types.size() < n) built.types.resize(n, JType::Unknown);
    if (built.block_of.size() < n) built.block_of.resize(n, UINT32_MAX);
    if (built.insn_of.size() < n) built.insn_of.resize(n, UINT32_MAX);
    if (built.spliced.size() < n) built.spliced.resize(n, 0);
    if (built.inline_paths.size() < n) built.inline_paths.resize(n);
}

uint32_t intersect_idom(uint32_t a, uint32_t b,
                        const std::vector<uint32_t>& rpo_index,
                        const std::vector<uint32_t>& idom);

// ---- shared sweep helpers ------------------------------------------------------

uint32_t replace_uses(ir::Graph& g, NodeId from, NodeId to) {
    if (from == to || from >= g.node_count()) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.node_count(); ++i) {
        Node& node = g.node(i);
        if (node.dead) continue;
        for (NodeId& in : node.data_inputs) {
            if (in == from) {
                in = to;
                ++n;
            }
        }
        if (node.control == from) {
            node.control = to;
            ++n;
        }
        if (node.effect_in == from) {
            node.effect_in = to;
            ++n;
        }
    }
    return n;
}

/// Replaces only DATA uses (result consumers), leaving effect chains alone.
uint32_t replace_data_uses(ir::Graph& g, NodeId from, NodeId to) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.node_count(); ++i) {
        Node& node = g.node(i);
        if (node.dead) continue;
        for (NodeId& in : node.data_inputs) {
            if (in == from) {
                in = to;
                ++n;
            }
        }
    }
    return n;
}

/// Redirects the effect chain: consumers of `from`'s effect now consume `to`.
uint32_t replace_effect_input(ir::Graph& g, NodeId from, NodeId to) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.node_count(); ++i) {
        Node& node = g.node(i);
        if (node.dead) continue;
        if (node.effect_in == from) {
            node.effect_in = to;
            ++n;
        }
    }
    return n;
}

void kill(ir::Graph& g, NodeId id) { g.node(id).dead = true; }

uint32_t kill_block_nodes(ir::Graph& g, const BuiltGraph& built,
                          uint32_t block) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.node_count(); ++i) {
        Node& node = g.node(i);
        if (node.dead) continue;
        if (built.block_of[i] == block) {
            node.dead = true;
            ++n;
        }
    }
    return n;
}

// ---- CFG utilities (recomputed after rewrites; shared with the J3 TU) -----------

void compute_dominators(const BuiltGraph& built, std::vector<uint32_t>& rpo,
                        std::vector<uint32_t>& idom) {
    const size_t n = built.blocks.size();
    rpo.clear();
    idom.assign(n, 0);
    std::vector<bool> visited(n, false);
    std::vector<uint32_t> post;
    // Iterative post-order DFS from entry (blocks may be unreachable after
    // rewrites: they append in tail order and stay unreachable).
    std::vector<std::pair<uint32_t, size_t>> stack;
    visited[built.entry_block] = true;
    stack.push_back({built.entry_block, 0});
    while (!stack.empty()) {
        auto& [b, i] = stack.back();
        if (i < built.blocks[b].succs.size()) {
            const uint32_t s = built.blocks[b].succs[i++];
            if (!visited[s]) {
                visited[s] = true;
                stack.push_back({s, 0});
            }
        } else {
            post.push_back(b);
            stack.pop_back();
        }
    }
    rpo.assign(post.rbegin(), post.rend());
    std::vector<uint32_t> rpo_index(n, UINT32_MAX);
    for (size_t i = 0; i < rpo.size(); ++i) rpo_index[rpo[i]] = i;
    idom[rpo[0]] = rpo[0];
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t bi = 1; bi < rpo.size(); ++bi) {
            const uint32_t b = rpo[bi];
            uint32_t nd = UINT32_MAX;
            for (const uint32_t pred : built.blocks[b].preds) {
                if (rpo_index[pred] == UINT32_MAX || pred == b) continue;
                nd = (nd == UINT32_MAX) ? pred : intersect_idom(pred, nd, rpo_index, idom);
            }
            if (nd != UINT32_MAX && idom[b] != nd) {
                idom[b] = nd;
                changed = true;
            }
        }
    }
}

uint32_t intersect_idom(uint32_t a, uint32_t b,
                        const std::vector<uint32_t>& rpo_index,
                        const std::vector<uint32_t>& idom) {
    while (a != b) {
        while (rpo_index[a] > rpo_index[b]) a = idom[a];
        while (rpo_index[b] > rpo_index[a]) b = idom[b];
    }
    return a;
}
// ---- driver -----------------------------------------------------------------------------

PipelineStats run_pipeline(ir::Graph& graph, BuiltGraph& built,
                           const PipelineBudget& budget) {
    PipelineStats stats;
    sync_built(graph, built);

    const auto budget_stop = [&](uint32_t stage) {
        if (graph.node_count() > budget.node_cap) {
            stats.budget_stop = stage;
            return true;
        }
        return false;
    };

    if (budget.pass_control & PassCanonicalize) {
        stats.folded_constants += canonicalize(graph, built);
    }
    if (budget_stop(1)) return stats;
    if (budget.pass_control & PassFoldConstants) {
        stats.folded_constants += fold_constants(graph, built);
    }
    if (budget_stop(2)) return stats;
    if (budget.pass_control & PassSCCP) {
        stats.blocks_unreachable += fold_branches_and_sweep(graph, built);
    }
    if (budget_stop(3)) return stats;
    if (budget.pass_control & PassDCE) {
        stats.nodes_dead += graph.eliminate_dead_nodes();
    }
    // CFG may have changed: recompute dominators for the scoped passes.
    compute_dominators(built, built.rpo, built.idom);
    if (budget.pass_control & PassValueNumber) {
        stats.value_numbered =
            value_number(graph, built, built.rpo, built.idom);
    }
    if (budget_stop(5)) return stats;
    if (budget.pass_control & PassGuards) {
        stats.guards_removed = optimize_guards(graph, built);
    }
    if (budget_stop(6)) return stats;
    if (budget.pass_control & PassInline) {
        // Module access flows through the BuiltGraph module pointer.
        if (built.module != nullptr) {
            stats.inlined_calls =
                inline_calls(graph, built, budget, *built.module);
            if (stats.inlined_calls != 0) {
                // Spliced code changes block contents; dominators are
                // unchanged (splices stay inside one block) but DCE and
                // folds see new material.
                stats.nodes_dead += graph.eliminate_dead_nodes();
            }
        }
    }
    if (budget_stop(7)) return stats;
    // Post-inline normalization: new pure chains fold, spliced guards dedup.
    if (budget.pass_control & PassCanonicalize) {
        canonicalize(graph, built);
    }
    if (budget.pass_control & PassFoldConstants) {
        fold_constants(graph, built);
    }
    if (budget.pass_control & PassGuards) {
        optimize_guards(graph, built);
    }
    if (budget.pass_control & PassDCE) {
        graph.eliminate_dead_nodes();
    }
    if (budget.pass_control & PassSpecializeIC) {
        stats.specialized_sites = specialize_ic(graph, built);
        if (stats.specialized_sites != 0 &&
            (budget.pass_control & PassGuards)) {
            optimize_guards(graph, built);  // dedup the synthetic guards
        }
    }
    if (budget_stop(10)) return stats;
    return stats;
}

std::vector<uint32_t> block_layout_order(const BuiltGraph& built) {
    // Stage 17: greedy hot-chain ordering. Start at entry; from each block
    // follow the most probable successor (permille from the T0 branch
    // profile); ties break by bytecode order (determinism, Rule 56). Blocks
    // left unchained append in bytecode order.
    const size_t n = built.blocks.size();
    std::vector<uint32_t> order;
    order.reserve(n);
    std::vector<bool> placed(n, false);
    uint32_t cur = built.entry_block;
    placed[cur] = true;
    order.push_back(cur);
    bool progress = true;
    while (progress) {
        progress = false;
        const Block& blk = built.blocks[cur];
        uint32_t best = UINT32_MAX;
        uint32_t best_prob = 0;
        for (const uint32_t s : blk.succs) {
            if (placed[s]) continue;
            const uint32_t prob =
                (blk.succs.size() == 2)
                    ? (s == blk.succs[0] ? blk.true_prob_permille
                                         : 1000 - blk.true_prob_permille)
                    : 1000;
            if (prob > best_prob ||
                (prob == best_prob && best != UINT32_MAX &&
                 built.blocks[s].bytecode_begin <
                     built.blocks[best].bytecode_begin)) {
                best = s;
                best_prob = prob;
            }
        }
        if (best != UINT32_MAX) {
            placed[best] = true;
            order.push_back(best);
            cur = best;
            progress = true;
            continue;
        }
        // Chain broken: start a new chain at the first unplaced block in
        // bytecode order.
        for (size_t b = 0; b < n; ++b) {
            if (!placed[b]) {
                placed[b] = true;
                order.push_back(static_cast<uint32_t>(b));
                cur = static_cast<uint32_t>(b);
                progress = true;
                break;
            }
        }
    }
    return order;
}

std::vector<uint32_t> dominance_placement(const ir::Graph& graph,
                                          const BuiltGraph& built) {
    // Two-phase schedule (docs/ir-son-ciog.md 5.2):
    //   1. LCA of use blocks — a Phi input counts as used on its incoming
    //      EDGE (the predecessor block), never at the Phi's own block, or
    //      loop-carried defs would hoist above the loop;
    //   2. sink: a definition may never land above its own inputs — the
    //      second pass sinks placed nodes down the dominator tree until the
    //      data dependencies are respected (node ids are topological, so a
    //      single pass in id order finalizes every placement).
    std::vector<uint32_t> depth(built.blocks.size(), 0);
    for (const uint32_t b : built.rpo) {
        if (b != built.entry_block && built.idom[b] != b) {
            depth[b] = depth[built.idom[b]] + 1;
        }
    }
    const auto lca = [&](uint32_t a, uint32_t b) {
        while (depth[a] > depth[b]) a = built.idom[a];
        while (depth[b] > depth[a]) b = built.idom[b];
        while (a != b) {
            a = built.idom[a];
            b = built.idom[b];
        }
        return a;
    };
    const auto dominates = [&](uint32_t a, uint32_t t) {
        while (depth[t] > depth[a]) t = built.idom[t];
        return t == a;
    };
    const auto child_toward = [&](uint32_t b, uint32_t t) {
        while (built.idom[t] != b) t = built.idom[t];
        return t;
    };
    std::vector<uint32_t> place(graph.node_count(), UINT32_MAX);
    for (uint32_t i = 0; i < graph.node_count(); ++i) {
        const Node& node = graph.node(i);
        if (node.dead) continue;
        place[i] = i < built.block_of.size() ? built.block_of[i] : UINT32_MAX;
    }
    // Phase 1: use-block LCAs. FrameState references count as uses of the
    // FS's own block (the deopt stub materializes from there — Rule 39).
    for (uint32_t i = 0; i < graph.node_count(); ++i) {
        const Node& node = graph.node(i);
        if (node.dead) continue;
        const uint32_t ub = i < built.block_of.size() ? built.block_of[i]
                                                      : UINT32_MAX;
        if (ub == UINT32_MAX) continue;
        if (node.kind == NodeKind::Phi) {
            // Input k is read on the edge from preds[k].
            const std::vector<uint32_t>& preds = built.blocks[ub].preds;
            for (size_t k = 0; k < node.data_inputs.size(); ++k) {
                if (k >= preds.size()) break;
                const NodeId in = node.data_inputs[k];
                if (in >= place.size()) continue;
                const uint32_t defb = place[in];
                const uint32_t pb = preds[k];
                if (defb == UINT32_MAX || defb == pb) continue;
                place[in] = lca(defb, pb);
            }
            continue;
        }
        for (const NodeId in : node.data_inputs) {
            if (in >= place.size()) continue;
            const uint32_t defb = place[in];
            if (defb == UINT32_MAX || defb == ub) continue;
            place[in] = lca(defb, ub);
        }
    }
    // Phase 2: sink below inputs (Phis are edge-defined and stay put).
    for (uint32_t i = 0; i < graph.node_count(); ++i) {
        const Node& node = graph.node(i);
        if (node.dead || node.kind == NodeKind::Phi) continue;
        if (i >= place.size() || place[i] == UINT32_MAX) continue;
        bool moved = true;
        while (moved) {
            moved = false;
            for (const NodeId in : node.data_inputs) {
                if (in >= place.size() || place[in] == UINT32_MAX) continue;
                uint32_t b = place[i];
                const uint32_t t = place[in];
                while (b != t && dominates(b, t)) {
                    b = child_toward(b, t);
                    place[i] = b;
                    moved = true;
                }
            }
        }
    }
    return place;
}

}  // namespace vortex::j2
