// Stage 7: redundant load elimination + store-to-load forwarding.
// Split from src/j3/passes_j3.cpp — one pass per file.
#include "passes_internal.hpp"

#include <algorithm>
#include <utility>
#include "vortex/support/containers.hpp"

namespace vortex::j3 {

using namespace vortex::j2;
using ir::Graph;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;
using BuiltGraph = j2::BuiltGraph;

// ---- stage 7: redundant load elimination + store-to-load forwarding --------
//
// Per block, along the EFFECT CHAIN (chain_order ranks, not id order —
// spliced callees carry high ids but execute at the call position): a Load
// whose (base, key) was already loaded, or whose (base, key) was stored, is
// replaced by the known value. A Store UPSERTS its (base, key) entry — the
// stored value replaces any earlier load/store value (a load before a store
// of the same slot must never forward past the store). Any other effectful
// node on the chain invalidates the table (conservative aliasing — Rule 55:
// correctness never depends on an optimization).
uint32_t redundant_load_elim(Graph& g, BuiltGraph& built) {
    uint32_t forwarded = 0;
    // Multiplicative mix prime (large, prime; deterministic — Rule 56):
    // distributes (node id, key) pairs across the per-block table buckets.
    constexpr uint64_t kPairMixPrime = 1'000'003ull;
    struct PairHash {
        uint64_t operator()(const std::pair<NodeId, int64_t>& p) const {
            return static_cast<uint64_t>(p.first) * kPairMixPrime +
                   static_cast<uint64_t>(p.second);
        }
    };
    for (const uint32_t b : built.rpo) {
        // The block's live effectful nodes in effect-chain order (the chain
        // is the program-order truth after splices — Rule 55; id order is
        // only program order within one origin).
        ChainOrder co;
        chain_order(g, built, b, co);
        std::vector<NodeId> chain;
        chain.reserve(co.chain.size());
        for (const NodeId id : co.chain) {
            if (!g.node(id).dead) chain.push_back(id);
        }
        support::FlatHashMap<std::pair<NodeId, int64_t>, NodeId, PairHash>
            known;
        for (const NodeId id : chain) {
            Node& n = g.node(id);
            int64_t key = 0;
            if (n.kind == NodeKind::Load && access_key_of(n, key) &&
                n.data_inputs.size() >= 1) {
                const NodeId* hit = known.find({n.data_inputs[0], key});
                if (hit != nullptr && !g.node(*hit).dead) {
                    j2::replace_data_uses(g, id, *hit);
                    j2::replace_effect_input(g, id, g.node(id).effect_in);
                    n.dead = true;
                    ++forwarded;
                    continue;
                }
                known.insert({n.data_inputs[0], key}, id);
            } else if (n.kind == NodeKind::Store && access_key_of(n, key) &&
                       n.data_inputs.size() >= 2) {
                // Store-to-load forwarding: later loads of the same slot
                // read the stored value. UPSERT, not first-wins insert:
                // FlatHashMap::insert keeps the existing value on a
                // duplicate key, which would forward a PRE-STORE load
                // across the store (Rule 18/107 miscompile).
                const NodeId value = n.data_inputs[1];
                if (NodeId* slot =
                        known.insert({n.data_inputs[0], key}, value)) {
                    *slot = value;
                }
            } else if (n.kind != NodeKind::Safepoint) {
                // Calls/allocates may alias anything: drop the table.
                // Safepoints are pure suspension points (no memory effect).
                known.clear();
            }
        }
    }
    if (forwarded != 0) {
        g.eliminate_dead_nodes();
        j2::sync_built(g, built);
    }
    return forwarded;
}

}  // namespace vortex::j3
