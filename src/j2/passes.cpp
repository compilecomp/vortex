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

namespace vortex::j2 {
namespace {

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

constexpr uint32_t kAuxMask = 0xFFFF;
constexpr uint32_t kShapeShift = 16;

bool is_pure_kind(NodeKind k) { return ir::is_pure(k); }
bool is_guard_kind(NodeKind k) { return ir::is_guard(k); }

int64_t smi_min() { return static_cast<int64_t>(0xFFFFFFFFFFFFFFFFull / 4 + 1); }
int64_t smi_max() { return static_cast<int64_t>(0xFFFFFFFFFFFFFFFFull / 4); }

// ---- growth helper -----------------------------------------------------------

/// Grows the BuiltGraph side tables to match the node store.
void sync_built(const ir::Graph& g, BuiltGraph& built) {
    const size_t n = g.node_count();
    if (built.types.size() < n) built.types.resize(n, JType::Unknown);
    if (built.block_of.size() < n) built.block_of.resize(n, UINT32_MAX);
    if (built.insn_of.size() < n) built.insn_of.resize(n, UINT32_MAX);
    if (built.spliced.size() < n) built.spliced.resize(n, 0);
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

// ---- CFG utilities (recomputed after rewrites) -----------------------------------

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

// ---- stage 3+4: constant folding ---------------------------------------------------

bool smi_const(const Node& c) {
    if (c.const_value & 1) return false;
    const int64_t raw = c.const_value >> 1;
    return raw >= smi_min() && raw <= smi_max();
}

double bits_to_f64(int64_t bits) {
    double d = 0.0;
    __builtin_memcpy(&d, &bits, sizeof(d));
    return d;
}

int64_t f64_to_bits(double d) {
    int64_t bits = 0;
    __builtin_memcpy(&bits, &d, sizeof(bits));
    return bits;
}

bool fold_one(ir::Graph& g, BuiltGraph& built, Node& node) {
    if (node.data_inputs.empty()) return false;
    for (const NodeId in : node.data_inputs) {
        if (g.node(in).kind != NodeKind::Const) return false;
    }
    const Node& lhs = g.node(node.data_inputs[0]);
    auto untag = [](const Node& c) { return c.const_value >> 1; };
    auto finish = [&](int64_t tagged, JType t) {
        const NodeId c = clone_const(g, built, node.id, tagged, t);
        replace_uses(g, node.id, c);
        return true;
    };
    switch (node.kind) {
    case NodeKind::Add: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        if (!smi_const(lhs) || !smi_const(rhs)) return false;
        int64_t r = 0;
        if (__builtin_add_overflow(untag(lhs), untag(rhs), &r)) return false;
        if (r < smi_min() || r > smi_max()) return false;  // would trap
        return finish(r << 1, JType::Smi);
    }
    case NodeKind::Sub: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        if (!smi_const(lhs) || !smi_const(rhs)) return false;
        int64_t r = 0;
        if (__builtin_sub_overflow(untag(lhs), untag(rhs), &r)) return false;
        if (r < smi_min() || r > smi_max()) return false;
        return finish(r << 1, JType::Smi);
    }
    case NodeKind::Mul: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        if (!smi_const(lhs) || !smi_const(rhs)) return false;
        int64_t r = 0;
        if (__builtin_mul_overflow(untag(lhs), untag(rhs), &r)) return false;
        if (r < smi_min() || r > smi_max()) return false;
        return finish(r << 1, JType::Smi);
    }
    case NodeKind::And: case NodeKind::Or: case NodeKind::Xor: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        if (!smi_const(lhs) || !smi_const(rhs)) return false;
        int64_t r = 0;
        switch (node.kind) {
        case NodeKind::And: r = lhs.const_value & rhs.const_value; break;
        case NodeKind::Or: r = lhs.const_value | rhs.const_value; break;
        default: r = lhs.const_value ^ rhs.const_value; break;
        }
        return finish(r, JType::Smi);
    }
    case NodeKind::Compare: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        bool r = false;
        switch (static_cast<CondCode>(node.aux)) {
        case CondCode::Eq: {
            if (lhs.const_value != rhs.const_value) return false;
            r = true;
            break;
        }
        case CondCode::Ne: {
            if (lhs.const_value != rhs.const_value) return false;
            r = false;
            break;
        }
        case CondCode::LtS: case CondCode::LeS: case CondCode::GtS:
        case CondCode::GeS: {
            if (!smi_const(lhs) || !smi_const(rhs)) return false;
            const int64_t x = untag(lhs), y = untag(rhs);
            switch (static_cast<CondCode>(node.aux)) {
            case CondCode::LtS: r = x < y; break;
            case CondCode::LeS: r = x <= y; break;
            case CondCode::GtS: r = x > y; break;
            default: r = x >= y; break;
            }
            break;
        }
        case CondCode::LtF: case CondCode::LeF: case CondCode::GtF:
        case CondCode::GeF: {
            const double x = bits_to_f64(lhs.const_value);
            const double y = bits_to_f64(rhs.const_value);
            switch (static_cast<CondCode>(node.aux)) {
            case CondCode::LtF: r = x < y; break;
            case CondCode::LeF: r = x <= y; break;
            case CondCode::GtF: r = x > y; break;
            default: r = x >= y; break;
            }
            break;
        }
        default:
            return false;
        }
        // Canonical boolean words (false = 0xB, true = 0xF — the same
        // encoding TaggedValue::boolean stores; a folded compare that
        // returned smi 0/1 would diverge from T0 on every
        // boolean-identity consumer, Rule 18/39).
        constexpr uint64_t kFoldTrueBits = 0xF;
        constexpr uint64_t kFoldFalseBits = 0xB;
        return finish(r ? kFoldTrueBits : kFoldFalseBits, JType::Bool);
    }
    case NodeKind::FAdd: case NodeKind::FSub: case NodeKind::FMul:
    case NodeKind::FDiv: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        const double x = bits_to_f64(lhs.const_value);
        const double y = bits_to_f64(rhs.const_value);
        double r = 0.0;
        switch (node.kind) {
        case NodeKind::FAdd: r = x + y; break;
        case NodeKind::FSub: r = x - y; break;
        case NodeKind::FMul: r = x * y; break;
        default: r = x / y; break;  // IEEE div-zero: inf/nan (Rule 110)
        }
        return finish(f64_to_bits(r), JType::Unknown);
    }
    default:
        return false;
    }
}

uint32_t fold_constants(ir::Graph& g, BuiltGraph& built) {
    // Named (Rule 72): sweep rounds until fixpoint — enough for chained
    // folds (a folded input enables a parent's fold) at J2 graph sizes.
    constexpr int kFoldSweeps = 4;
    uint32_t folded = 0;
    for (int sweep = 0; sweep < kFoldSweeps; ++sweep) {
        bool changed = false;
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            Node& node = g.node(id);
            if (node.dead || !is_pure_kind(node.kind)) continue;
            if (node.kind == NodeKind::Const ||
                node.kind == NodeKind::Parameter) {
                continue;
            }
            if (fold_one(g, built, node)) {
                node.dead = true;
                ++folded;
                changed = true;
            }
        }
        if (!changed) break;
    }
    return folded;
}

// ---- stage 5+6: branch folding + reachability + DCE --------------------------------

uint32_t fold_branches_and_sweep(ir::Graph& g, BuiltGraph& built) {
    uint32_t changed = 0;
    for (uint32_t b = 0; b < built.blocks.size(); ++b) {
        Block& blk = built.blocks[b];
        if (blk.control_end == kNoNode) continue;
        Node& end = g.node(blk.control_end);
        if (end.dead || end.kind != NodeKind::If) continue;
        if (end.data_inputs.empty()) continue;
        const Node& cond = g.node(end.data_inputs[0]);
        if (cond.kind != NodeKind::Const) continue;
        if (blk.succs.size() < 2) continue;
        // Truthiness on the canonical tagged word (T0 TaggedValue::truthy,
        // Rule 18): even words are Smis (truthy iff the word is nonzero);
        // odd words are the special/heap encodings — false (0xB), null
        // (0x3) and undefined (0x7) are falsey, true (0xF) is truthy, and
        // the builder never emits heap-pointer constants.
        const uint64_t w = static_cast<uint64_t>(cond.const_value);
        const bool truthy = (w & 1) == 0 ? (w != 0) : (w == 0xF);
        const bool take_target = truthy == blk.target_is_true_edge;
        const uint32_t keep = take_target ? blk.succs[0] : blk.succs[1];
        const uint32_t drop = take_target ? blk.succs[1] : blk.succs[0];
        end.kind = NodeKind::End;
        end.data_inputs.clear();
        auto& succs = blk.succs;
        succs.erase(std::remove(succs.begin(), succs.end(), drop),
                    succs.end());
        auto& preds_keep = built.blocks[keep].preds;
        if (std::find(preds_keep.begin(), preds_keep.end(), b) ==
            preds_keep.end()) {
            preds_keep.push_back(b);
        }
        auto& preds_drop = built.blocks[drop].preds;
        preds_drop.erase(std::remove(preds_drop.begin(), preds_drop.end(), b),
                         preds_drop.end());
        kill(g, end.id);
        ++changed;
    }
    // Unreachable-block sweep.
    std::vector<bool> reach(built.blocks.size(), false);
    std::vector<uint32_t> work{built.entry_block};
    reach[built.entry_block] = true;
    while (!work.empty()) {
        const uint32_t b = work.back();
        work.pop_back();
        for (const uint32_t s : built.blocks[b].succs) {
            if (!reach[s]) {
                reach[s] = true;
                work.push_back(s);
            }
        }
    }
    for (uint32_t b = 0; b < built.blocks.size(); ++b) {
        if (!reach[b]) {
            changed += kill_block_nodes(g, built, b);
            built.blocks[b].bytecode_end =
                built.blocks[b].bytecode_begin;  // diagnostics only
            ++changed;
        }
    }
    return changed;
}

// ---- stages 7+8: scoped value numbering ----------------------------------------------

struct VnKey {
    uint32_t kind;
    uint32_t aux;
    int64_t const_value;
    uint32_t input_count;
    NodeId inputs[4];
    bool operator==(const VnKey& o) const {
        if (kind != o.kind || aux != o.aux || const_value != o.const_value ||
            input_count != o.input_count) {
            return false;
        }
        for (uint32_t i = 0; i < input_count && i < 4; ++i) {
            if (inputs[i] != o.inputs[i]) return false;
        }
        return true;
    }
};

struct VnHash {
    size_t operator()(const VnKey& k) const {
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(k.kind);
        mix(k.aux);
        mix(static_cast<uint64_t>(k.const_value));
        mix(k.input_count);
        for (uint32_t i = 0; i < k.input_count && i < 4; ++i) {
            mix(k.inputs[i]);
        }
        return static_cast<size_t>(h);
    }
};

VnKey vn_key(const Node& n) {
    VnKey k{};
    k.kind = static_cast<uint32_t>(n.kind);
    k.aux = n.aux;
    k.const_value = n.const_value;
    k.input_count = static_cast<uint32_t>(n.data_inputs.size());
    for (uint32_t i = 0; i < k.input_count && i < 4; ++i) {
        k.inputs[i] = n.data_inputs[i];
    }
    return k;
}

uint32_t value_number(ir::Graph& g, BuiltGraph& built,
                      const std::vector<uint32_t>& rpo,
                      const std::vector<uint32_t>& idom) {
    // Scoped along the dominator tree: entries carry their tree depth and
    // are popped when the walk leaves their subtree.
    struct Entry {
        VnKey key;
        NodeId node;
        uint32_t depth;
    };
    std::vector<Entry> scope;
    std::vector<uint32_t> depth(built.blocks.size(), 0);
    for (const uint32_t b : rpo) {
        if (b == built.entry_block) {
            depth[b] = 0;
        } else if (idom[b] != b) {
            depth[b] = depth[idom[b]] + 1;
        }
    }
    uint32_t replaced = 0;
    for (const uint32_t b : rpo) {
        while (!scope.empty() && scope.back().depth > depth[b]) {
            scope.pop_back();
        }
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            Node& node = g.node(id);
            if (node.dead || !is_pure_kind(node.kind)) continue;
            if (node.kind == NodeKind::Const ||
                node.kind == NodeKind::Parameter) {
                continue;
            }
            if (built.block_of[id] != b) continue;
            const VnKey key = vn_key(node);
            NodeId hit = kNoNode;
            for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
                if (it->key == key) {
                    hit = it->node;
                    break;
                }
            }
            if (hit != kNoNode) {
                // Dominance validation: the scoped pop only fires when the
                // walk leaves a SUBTREE; same-depth siblings inherit each
                // other's entries. A hit from a non-dominating block is
                // unsound (its def may not execute before this use), so
                // every hit must sit on b's idom chain.
                bool dominated = false;
                const uint32_t hit_block = built.block_of[hit];
                for (uint32_t x = b;
                     x < built.blocks.size() && x != built.entry_block;
                     x = idom[x]) {
                    if (x == hit_block) {
                        dominated = true;
                        break;
                    }
                    if (idom[x] == x) break;
                }
                if (hit_block == b) dominated = true;
                if (dominated) {
                    replace_uses(g, node.id, hit);
                    node.dead = true;
                    ++replaced;
                    continue;
                }
                // Non-dominating: keep this node's own entry (fall through
                // to the scope push below).
            }
            scope.push_back({key, node.id, depth[b]});
        }
        // Scope pop happens on the next block's entry (the depth test above)
        // — entries of this block's own depth survive to dominated siblings
        // exactly when they are also dominator-tree children, which the
        // depth test enforces. Origin note: within one block the walk is id
        // order, but a replacement is only sound because the EMITTER
        // schedules by data dependencies — a rewired use gains a
        // hit->use edge, so plan_emission orders the hit first regardless
        // of splice origin (the spliced stamp guards earlier-id reasoning
        // like optimize_guards, not dependency-driven scheduling).
        (void)idom;
    }
    return replaced;
}

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

// ---- stage 16: IC specialization --------------------------------------------------------

/// Resolves a T0 IC klass id to the Klass* the guard compares against
/// (same authoritative id() lookup as the J1 strengthener).
const void* klass_addr_by_id(const std::vector<void*>& klass_addrs,
                             uint32_t klass_id) {
    for (const void* addr : klass_addrs) {
        if (addr != nullptr &&
            static_cast<const Klass*>(addr)->id() == klass_id) {
            return addr;
        }
    }
    return nullptr;
}

/// Stage 16 — small IC specialization: a Monomorphic field site becomes a
/// ClassGuard (profiled klass) + raw offset access; the generic helper path
/// remains as the deopt target (Rule 34: specialization keeps a fallback).
/// Effect position is preserved exactly (no reordering).
uint32_t specialize_ic(ir::Graph& g, BuiltGraph& built) {
    if (built.profiles == nullptr || built.ics == nullptr ||
        built.klass_addrs == nullptr) {
        return 0;
    }
    uint32_t specialized = 0;
    for (uint32_t id = 0; id < g.node_count(); ++id) {
        Node& node = g.node(id);
        if (node.dead) continue;
        const bool is_load = node.kind == NodeKind::Load;
        const bool is_store = node.kind == NodeKind::Store;
        if (!is_load && !is_store) continue;
        if (static_cast<AccessKind>(node.aux) != AccessKind::Field) continue;
        const uint32_t insn = built.insn_of[node.id];
        if (insn >= built.ics->size()) continue;
        const ugb::IcSlot& ic = (*built.ics)[insn];
        if (ic.state != ugb::IcState::Monomorphic) continue;
        const void* klass = klass_addr_by_id(*built.klass_addrs,
                                             ic.mono.klass_or_shape_id);
        if (klass == nullptr) continue;
        // The access node's LAST data input is its FrameState (the builder
        // attaches it so synthetic guards satisfy Rule 31).
        if (node.data_inputs.size() < 2) continue;
        NodeId fs = node.data_inputs.back();
        NodeId obj = node.data_inputs[0];
        const NodeId guard = g.add_aux(NodeKind::ClassGuard, {obj, fs},
                                       ic.mono.klass_or_shape_id,
                                       built.blocks[built.block_of[node.id]]
                                           .region);
        sync_built(g, built);
        built.block_of[guard] = built.block_of[node.id];
        built.types[guard] = JType::Ref;
        built.insn_of[guard] = insn;
        // The synthesized guard needs its own deopt record: register it in
        // the guard list the deopt-record builder walks (Rule 30 — every
        // speculative node carries its state-exact resume).
        built.guards.push_back(guard);
        // Raw access at the profiled slot (byte offset 16 + 8*slot).
        const int64_t byte_off =
            static_cast<int64_t>(sizeof(ObjectHeader)) +
            8 * static_cast<int64_t>(ic.mono.target);
        if (is_load) {
            const NodeId raw = g.add_aux(
                NodeKind::Load, {guard},
                static_cast<uint32_t>(AccessKind::RawOffset),
                node.control, node.effect_in);
            sync_built(g, built);
            g.node(raw).const_value = byte_off;
            built.block_of[raw] = built.block_of[node.id];
            built.types[raw] = built.types[node.id];
            built.insn_of[raw] = insn;
            replace_data_uses(g, node.id, raw);
            replace_effect_input(g, node.id, raw);
        } else {
            const NodeId value = node.data_inputs[1];
            const NodeId raw = g.add_aux(
                NodeKind::Store, {guard, value},
                static_cast<uint32_t>(AccessKind::RawOffset),
                node.control, node.effect_in);
            sync_built(g, built);
            g.node(raw).const_value = byte_off;
            built.block_of[raw] = built.block_of[node.id];
            built.insn_of[raw] = insn;
            replace_effect_input(g, node.id, raw);
        }
        kill(g, node.id);
        ++specialized;
    }
    return specialized;
}

// ---- stages 14+15: bounded inlining ------------------------------------------------------

uint32_t param_index_of(const ir::Graph& cg, NodeId param_id) {
    uint32_t idx = 0;
    for (const Node& n : cg.nodes()) {
        if (n.id == param_id) return idx;
        if (n.kind == NodeKind::Parameter) ++idx;
    }
    return idx;
}

bool builds_single_block(const ugb::UGBModule& module,
                         const ugb::UGBMethod& callee, size_t node_cap,
                         std::unique_ptr<BuiltGraph>& out) {
    GraphBuilderParams cb{};
    cb.module = &module;
    cb.method = &callee;
    cb.node_cap = node_cap;
    BuildResult cr = build_graph(cb);
    if (!cr.ok) return false;
    if (cr.out->blocks.size() != 1) return false;
    out = std::move(cr.out);
    return true;
}

uint32_t inline_calls(ir::Graph& g, BuiltGraph& built,
                      const PipelineBudget& budget,
                      const ugb::UGBModule& module) {
    // Bytecode pc AFTER the instruction at `pc` in method `method_id` (the
    // call-return continuation for the deopt record, Rule 113).
    // decode_at advances its offset argument BY REFERENCE past the decoded
    // instruction (ugb::InstructionStream contract), so returning `cur`
    // after the decode IS the next pc.
    const auto next_pc_after = [&module](uint32_t method_id,
                                         uint32_t pc) -> uint32_t {
        if (method_id >= module.method_table.size()) return pc;
        const ugb::UGBMethod& m = module.method_table[method_id];
        ugb::InstructionStream stream(m.code.data(), m.code.size());
        size_t cur = pc;
        ugb::Instruction ins;
        if (!stream.decode_at(cur, ins)) return pc;
        return static_cast<uint32_t>(cur);
    };
    uint32_t inlined = 0;
    for (uint32_t depth = 0; depth < budget.inline_depth_cap; ++depth) {
        bool any = false;
        for (uint32_t call_id = 0; call_id < g.node_count(); ++call_id) {
            Node& call = g.node(call_id);
            if (call.dead || call.kind != NodeKind::Call) continue;
            const uint32_t shape = (call.aux >> kShapeShift) & 0x3;
            if (shape != static_cast<uint32_t>(CallShape::Direct)) continue;
            if (call.data_inputs.size() < 2) continue;
            const uint32_t callee_token = call.aux & kAuxMask;
            if (callee_token >= module.runtime.method_resolution.size()) {
                continue;
            }
            const int64_t resolved =
                module.runtime.method_resolution[callee_token];
            if (resolved < 0 ||
                static_cast<size_t>(resolved) >= module.method_table.size()) {
                continue;
            }
            const ugb::UGBMethod& callee =
                module.method_table[static_cast<size_t>(resolved)];
            if (callee.id == built.method_id) continue;  // no recursion
            std::unique_ptr<BuiltGraph> callee_built;
            if (!builds_single_block(module, callee,
                                     budget.inline_callee_node_cap,
                                     callee_built)) {
                if (std::getenv("VORTEX_J2_TRACE")) {
                    fprintf(stderr, "[j2-inline] skip single-block: %s\n",
                            callee.name.c_str());
                }
                continue;
            }
            const ir::Graph& cg = *callee_built->graph;
            if (g.node_count() + cg.node_count() > budget.node_cap) {
                continue;  // graceful: keep the call, stop growing
            }
            // Arity check: call inputs = [args..., FrameState].
            const size_t argc = call.data_inputs.size() - 1;
            uint32_t param_count = 0;
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                if (cg.node(k).kind == NodeKind::Parameter) ++param_count;
            }
            if (param_count < argc) {
                // Malformed: the callee cannot name its own arguments.
                if (std::getenv("VORTEX_J2_TRACE")) {
                    fprintf(stderr,
                            "[j2-inline] skip arity: %s params=%u argc=%zu\n",
                            callee.name.c_str(), param_count, argc);
                }
                continue;
            }
            // params > argc are the callee's register-file entries beyond
            // the argument window (the entry ABI materializes every
            // register; T0 initializes the tail to undefined). They splice
            // as undefined constants, never as caller values.
            if (built.inlined_sites >= budget.inline_site_cap) break;

            // ---- splice ----------------------------------------------------
            const uint32_t base = g.node_count();
            const uint32_t call_block = built.block_of[call.id];
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                std::vector<NodeId> inputs;
                inputs.reserve(src.data_inputs.size());
                for (const NodeId in : src.data_inputs) {
                    inputs.push_back(static_cast<NodeId>(base + in));
                }
                const NodeId ctrl =
                    src.control != kNoNode
                        ? static_cast<NodeId>(base + src.control)
                        : kNoNode;
                const NodeId eff =
                    src.effect_in != kNoNode
                        ? static_cast<NodeId>(base + src.effect_in)
                        : kNoNode;
                g.add_vec(src.kind, inputs, ctrl, eff);
            }
            sync_built(g, built);
            // Origin stamp: every spliced node is marked so later passes
            // never treat id order as program order across the splice line
            // (optimize_guards; any future earlier-id-means-earlier pass
            // must consult this too).
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                built.spliced[base + k] = 1;
            }
            // Map + fix payload words.
            for (const Node& src : cg.nodes()) {
                const NodeId copy = static_cast<NodeId>(base + src.id);
                Node& cn = g.node(copy);
                cn.aux = src.aux;
                cn.const_value = src.const_value;
                built.types[copy] =
                    src.id < callee_built->types.size()
                        ? callee_built->types[src.id]
                        : JType::Unknown;
                built.block_of[copy] = call_block;
                built.insn_of[copy] = built.insn_of[call.id];
                if (src.kind == NodeKind::Parameter) {
                    // vreg order == parameter creation order.
                    const uint32_t pidx = param_index_of(cg, src.id);
                    NodeId arg = kNoNode;
                    if (pidx < argc) {
                        arg = call.data_inputs[pidx];
                    } else {
                        // Beyond the argument window: the callee's entry
                        // reads undefined (the J2 entry prologue writes the
                        // same pattern for registers past argc).
                        arg = clone_const(g, built, call.id,
                                          TaggedValue::undefined().raw(),
                                          JType::Unknown);
                    }
                    if (arg != kNoNode) {
                        replace_uses(g, copy, arg);
                        cn.dead = true;
                    }
                }
                if (src.kind == NodeKind::Start) {
                    cn.control = kNoNode;
                    cn.dead = true;  // control flows from the call block
                }
            }
            // The callee's guards keep their deopt semantics in the caller:
            // register every spliced guard so the deopt-record builder sees
            // it (Rule 30 — a guard without a record cannot resume).
            for (const NodeId gg : callee_built->guards) {
                built.guards.push_back(static_cast<NodeId>(base + gg));
            }
            // Region control -> the call's block region; effect entry ->
            // the call's effect input.
            const Node& callee_start = cg.nodes()[0];
            (void)callee_start;
            // The callee is single-block: its Start's region child is the
            // body region (id from the callee layout) — relink to the call
            // block's region.
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                if (src.kind == NodeKind::Region && src.control != kNoNode &&
                    cg.node(src.control).kind == NodeKind::Start) {
                    g.node(base + src.id).control =
                        built.blocks[call_block].region;
                }
            }
            // Effect chain entry: the builder gives the callee's FIRST
            // effect node effect_in = kNoNode (nothing precedes it inside
            // the callee). Wire it to the call's effect_in so the spliced
            // body sits on the caller's chain — without this, the caller's
            // prior stores/loads are unordered against the spliced ones
            // (Rule 55: side-effect order is part of the pass contract).
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                if (ir::is_effectful(src.kind) && src.effect_in == kNoNode) {
                    g.node(base + src.id).effect_in = call.effect_in;
                    break;  // single-block callee: exactly one chain entry
                }
            }
            // Find the call's effect consumers and splice them to the
            // callee's last live effect.
            NodeId callee_last_effect = kNoNode;
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                if (ir::is_effectful(src.kind)) {
                    callee_last_effect = static_cast<NodeId>(base + src.id);
                }
            }
            if (callee_last_effect != kNoNode) {
                replace_effect_input(g, call.id, callee_last_effect);
            } else {
                replace_effect_input(g, call.id, call.effect_in);
            }
            // Return value -> the call's data users.
            NodeId result = kNoNode;
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                if (src.kind == NodeKind::Return) {
                    result = static_cast<NodeId>(base + src.data_inputs[0]);
                }
            }
            if (result == kNoNode) {
                // Unit return: substitute the null word.
                result = g.add_const(static_cast<int64_t>(0x3));
                built.types.resize(g.node_count(), JType::Unknown);
                built.types[result] = JType::Null;
                built.block_of.resize(g.node_count(), UINT32_MAX);
                built.block_of[result] = call_block;
            }
            replace_data_uses(g, call.id, result);
            // Guard/call/frame nodes inside the callee keep their deopt
            // FrameState (last data input, remapped); record the CALLER
            // continuation so the deopt generator emits the frame chain
            // (Rule 113). FrameStates are mapped so a depth-2 guard walks
            // guard.fs -> middle fs -> root fs through the same table.
            const NodeId caller_fs = call.data_inputs.back();
            const uint32_t caller_method =
                caller_fs != kNoNode
                    ? static_cast<uint32_t>(
                          std::max<int64_t>(g.node(caller_fs).const_value, 0))
                    : built.method_id;
            const uint32_t return_pc =
                next_pc_after(caller_method, g.node(caller_fs).aux);
            const BuiltGraph::InlineCallerInfo continuation{
                caller_fs,
                static_cast<uint32_t>(call.const_value), return_pc};
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                if (ir::is_guard(src.kind) || ir::is_effectful(src.kind) ||
                    src.kind == NodeKind::FrameState) {
                    built.inline_caller_frame[base + src.id] = continuation;
                }
            }
            kill(g, call.id);
            ++built.inlined_sites;
            ++inlined;
            any = true;
        }
        if (!any) break;
    }
    return inlined;
}

}  // namespace

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
