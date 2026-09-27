// Stages 7+8: scoped value numbering along the dominator tree (pure
// nodes join; same-block redundant loads with no intervening effect).
// Split from src/j2/passes.cpp — one pass per file.
#include "vortex/j2/passes.hpp"

#include <algorithm>
#include <cstring>

#include "vortex/support/containers.hpp"

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

}  // namespace vortex::j2
