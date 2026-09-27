// Stages 11/26/41/42: guard redundancy by dominance.
// Split from src/j3/passes_j3.cpp — one pass per file.
#include "passes_internal.hpp"

#include <cstdint>
#include <utility>
#include "vortex/support/containers.hpp"

namespace vortex::j3 {

using namespace vortex::j2;
using ir::Graph;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;
using BuiltGraph = j2::BuiltGraph;

// ---- stages 11/26/41/42: guard redundancy by dominance -----------------------
//
// A guard whose (value, discriminator) pair is already checked by a
// dominating guard of the same shape is dead — the later check can never
// fail that the earlier one did not (Rule 31's propagation arm).
uint32_t eliminate_dominated_guards(Graph& g, BuiltGraph& built) {
    std::vector<uint32_t> rpo;
    std::vector<uint32_t> idom;
    j2::compute_dominators(built, rpo, idom);
    std::vector<uint32_t> rpo_index(g.node_count(), UINT32_MAX);
    for (size_t i = 0; i < rpo.size(); ++i) rpo_index[rpo[i]] = i;
    auto dominates = [&](uint32_t a, uint32_t b) {
        while (b != UINT32_MAX) {
            if (b == a) return true;
            if (b == 0) return false;
            b = idom[b];
        }
        return false;
    };
    uint32_t killed = 0;
    // Guards in program order; each remembers its block. A later guard is
    // killed when an earlier same-shape guard dominates its block.
    // Origin discipline (the j2 optimize_guards pattern): the key carries
    // the node's SPLICE STAMP. Id order is program order only WITHIN one
    // origin — after an inline splice, callee nodes carry high ids but
    // execute at the call position, so a caller guard may never prove a
    // spliced one (the spliced guard can fail before the caller's pc even
    // runs; its own deopt state is the only exact one — Rules 39/41/107).
    struct Key {
        NodeId value;
        uint32_t kind;  // NodeKind for class/shape guards, GuardKind for
                        // TypeGuard
        uint32_t aux;   // klass id / guard subtype
        uint32_t origin;  // splice stamp (0 = builder-created)
        bool operator==(const Key& o) const {
            return value == o.value && kind == o.kind && aux == o.aux &&
                   origin == o.origin;
        }
    };
    struct PairHash {
        uint64_t operator()(const Key& k) const {
            return static_cast<uint64_t>(k.value) * 1'000'003ull ^
                   (static_cast<uint64_t>(k.kind) << 21) ^
                   static_cast<uint64_t>(k.aux) ^
                   (static_cast<uint64_t>(k.origin) << 42);
        }
    };
    // key -> entry-list index (the map slot stays word-packed, Rule 50);
    // the entry lists live in a dense side vector.
    support::FlatHashMap<Key, uint32_t, PairHash> seen;
    std::vector<std::vector<std::pair<uint32_t, NodeId>>> seen_lists;
    for (const NodeId gid : built.guards) {
        Node& n = g.node(gid);
        if (n.dead) continue;
        if (!ir::is_guard(n.kind)) continue;
        if (n.data_inputs.empty()) continue;
        // OverflowGuard and BoundsGuard are range checks, not identity
        // checks — dominance kills need proof shape, keep them.
        if (n.kind == NodeKind::OverflowGuard ||
            n.kind == NodeKind::BoundsGuard) {
            continue;
        }
        const uint32_t kind = n.kind == NodeKind::TypeGuard
                                  ? static_cast<uint32_t>(n.aux) + 0x10000
                                  : static_cast<uint32_t>(n.kind);
        const uint32_t origin =
            gid < built.spliced.size() ? built.spliced[gid] : 0;
        Key key{n.data_inputs[0], kind, n.aux, origin};
        const uint32_t* list_idx = seen.find(key);
        if (list_idx == nullptr) {
            seen.insert(key, static_cast<uint32_t>(seen_lists.size()));
            seen_lists.emplace_back();
            list_idx = seen.find(key);
        }
        std::vector<std::pair<uint32_t, NodeId>>& entries =
            seen_lists[*list_idx];
        const uint32_t block = built.block_of[gid];
        bool dominated = false;
        NodeId dominator = ir::kNoNode;
        for (const auto& [prev_block, prev_guard] : entries) {
            if (g.node(prev_guard).dead) continue;
            if (dominates(prev_block, block)) {
                dominated = true;
                dominator = prev_guard;
                break;
            }
        }
        if (dominated) {
            // The proven value is the earlier guard's output; consumers of
            // this guard's proven value rewire to the dominator's.
            j2::replace_data_uses(g, gid, dominator);
            n.dead = true;
            ++killed;
            continue;
        }
        entries.push_back({block, gid});
    }
    if (killed != 0) {
        g.eliminate_dead_nodes();
        j2::sync_built(g, built);
    }
    return killed;
}

}  // namespace vortex::j3
