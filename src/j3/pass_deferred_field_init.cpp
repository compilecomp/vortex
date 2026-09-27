// Stage 19: deferred field initialization (dead overriding stores).
// Split from src/j3/passes_j3.cpp — one pass per file.
#include "passes_internal.hpp"

#include <algorithm>
#include <unordered_map>

namespace vortex::j3 {

using namespace vortex::j2;
using ir::Graph;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;
using BuiltGraph = j2::BuiltGraph;

// ---- stage 19: deferred field initialization ----------------------------------
//
// A store to an allocation field that a LATER store to the same (base, key)
// in the same block overwrites, with NO intervening effectful node, is never
// observable -> die (the initialization defers to the winning store). The
// scan walks the EFFECT CHAIN (chain_order ranks, not id order — Rule 55)
// and any non-Store node between the two stores breaks it: a Load reads, a
// Call may read through its arguments, a WriteBarrier publishes, and a
// Safepoint suspension observes heap state on deopt (Rules 41/45/107).
uint32_t deferred_field_init(Graph& g, BuiltGraph& built) {
    uint32_t killed = 0;
    for (const uint32_t b : built.rpo) {
        std::unordered_map<NodeId, uint32_t> rank;
        chain_order(g, built, b, rank);
        std::vector<NodeId> chain;
        chain.reserve(rank.size());
        for (const auto& [id, r] : rank) {
            if (!g.node(id).dead) chain.push_back(id);
        }
        std::sort(chain.begin(), chain.end(),
                  [&](NodeId a, NodeId c) { return rank[a] < rank[c]; });
        for (size_t i = 0; i < chain.size(); ++i) {
            Node& a = g.node(chain[i]);
            if (a.dead || a.kind != NodeKind::Store) continue;
            int64_t key_a = 0;
            if (!access_key_of(a, key_a)) continue;
            for (size_t j = i + 1; j < chain.size(); ++j) {
                Node& c = g.node(chain[j]);
                if (c.dead) continue;
                // ANY intervening non-Store effectful node makes the earlier
                // store observable (or the pair unrelated) — stop scanning.
                if (c.kind != NodeKind::Store) break;
                int64_t key_c = 0;
                if (!access_key_of(c, key_c)) continue;
                if (c.data_inputs.empty() ||
                    c.data_inputs[0] != a.data_inputs[0] || key_c != key_a) {
                    // Different slot: keep scanning past it (a Store is not
                    // an observer of `a`'s slot).
                    continue;
                }
                // Same base value + same key, no intervening effect: the
                // first store is dead (Rule 55: the effect chain re-links).
                j2::replace_effect_input(g, chain[i],
                                         g.node(chain[i]).effect_in);
                a.dead = true;
                ++killed;
                break;
            }
        }
    }
    if (killed != 0) {
        g.eliminate_dead_nodes();
        j2::sync_built(g, built);
    }
    return killed;
}

}  // namespace vortex::j3
