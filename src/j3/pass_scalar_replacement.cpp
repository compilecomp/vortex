// Stages 12 + 16: escape analysis + scalar replacement over the
// merged graph (XLEA phases 1-2; single-block M3 scope).
// Split from src/j3/passes_j3.cpp — one pass per file.
#include "passes_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include "vortex/runtime/object_model.hpp"

namespace vortex::j3 {

using namespace vortex::j2;
using ir::Graph;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;
using BuiltGraph = j2::BuiltGraph;

// ---- stages 12 + 16: escape analysis + scalar replacement ---------------------
//
// The XLEA phases 1-2 over the MERGED graph (docs/xlea.md sections 1-2):
// after cross-language inlining, an allocation that is only field-stored,
// field-read and identity-guarded within ONE block does not escape — it is
// scalar-replaced into its fields, and the allocation (and any interop
// dispatch lowered around it) vanishes.
//
// FrameState references do NOT escape: the deopt record gains a
// rematerialization descriptor (klass token + field window slots) so the
// runtime rebuilds the object state-exactly on deopt (Rule 39; xlea.md
// section 5's materialization machinery, extended with the klass token).
// Every FrameState that references the allocation must also reference every
// field VALUE (the window carries them) — otherwise the allocation escapes.
//
// M3 scope boundary (documented in docs/tier-j3.md): single-block
// replacements. Multi-block and speculative (profile-licensed) replacement
// with runtime G1-G5 guards land with J4's persistent-profile machinery
// (docs/xlea.md sections 3/5).

/// Effect-chain order within one block: nodes earlier on the chain get
/// smaller ranks. Id order is NOT program order once a pass appends nodes
/// (stage 16 appends specialized accesses behind everything) — the chain
/// is the program-order truth (Rule 55).
void chain_order(const Graph& g, const BuiltGraph& built, uint32_t block,
                 std::unordered_map<NodeId, uint32_t>& rank) {
    // Depth = backward-walk length to the block's chain head.
    std::vector<NodeId> nodes;
    for (uint32_t id = 0; id < g.node_count(); ++id) {
        const Node& n = g.node(id);
        if (n.dead || built.block_of[id] != block) continue;
        if (!ir::is_effectful(n.kind)) continue;
        nodes.push_back(id);
    }
    for (const NodeId id : nodes) {
        // True depth: hop count to the block's chain head (the first
        // effect_in that leaves the block). Dead nodes stay on the chain
        // and are counted — the chain is the program-order truth.
        uint32_t depth = 0;
        NodeId cur = g.node(id).effect_in;
        while (cur != ir::kNoNode && built.block_of[cur] == block) {
            ++depth;
            cur = g.node(cur).effect_in;
            if (depth > g.node_count()) break;  // cycle guard (defensive)
        }
        rank[id] = depth;
    }
    // Depths share the same head but the walk above stops at already-ranked
    // nodes; normalize by sorting and re-ranking (deterministic).
    std::vector<NodeId> ordered(nodes.begin(), nodes.end());
    std::sort(ordered.begin(), ordered.end(),
              [&](NodeId a, NodeId b) { return rank[a] < rank[b]; });
    for (uint32_t i = 0; i < ordered.size(); ++i) rank[ordered[i]] = i;
}

uint32_t scalar_replace(Graph& g, BuiltGraph& built) {
    // Alias map: allocation -> its identity guards (the guard's proven
    // value aliases the allocation for field accesses).
    const bool trace = std::getenv("VORTEX_EA_TRACE") != nullptr;
    uint32_t replaced = 0;
    if (trace) {
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            const Node& n = g.node(id);
            if (!n.dead && n.kind == NodeKind::Allocate) {
                std::fprintf(stderr, "[ea] visit alloc %u aux=%u dead=0\n",
                             id, n.aux);
            }
        }
    }
    for (uint32_t id = 0; id < g.node_count(); ++id) {
        const Node& alloc = g.node(id);
        if (alloc.dead || alloc.kind != NodeKind::Allocate) continue;
        // Object allocations only (aux = klass token; 1 = array, 2 = boxed
        // f64 — different layouts, not in the M3 replacement domain).
        if (alloc.aux == 1 || alloc.aux == 2) continue;
        if (built.klass_addrs == nullptr ||
            alloc.aux >= built.klass_addrs->size()) {
            continue;
        }

        // Collect the allocation's alias set (guards proving its identity).
        std::vector<NodeId> aliases{id};
        for (uint32_t gid = 0; gid < g.node_count(); ++gid) {
            const Node& gn = g.node(gid);
            if (gn.dead || !ir::is_guard(gn.kind)) continue;
            if (gn.kind == NodeKind::OverflowGuard ||
                gn.kind == NodeKind::BoundsGuard) {
                continue;
            }
            if (!gn.data_inputs.empty() && gn.data_inputs[0] == id) {
                aliases.push_back(gid);
            }
        }

        // Field accesses keyed by base alias + access key.
        std::vector<std::pair<NodeId, int64_t>> stores;
        std::vector<NodeId> loads;
        bool esc = false;
        uint32_t anchor_block = built.block_of[id];
        for (const NodeId alias : aliases) {
            for (uint32_t nid = 0; nid < g.node_count(); ++nid) {
                const Node& n = g.node(nid);
                if (n.dead || nid == id) continue;
                // Identity guards of the alias set are handled as aliases
                // (their own users are scanned under their alias entry).
                if (ir::is_guard(n.kind) &&
                    n.kind != NodeKind::OverflowGuard &&
                    n.kind != NodeKind::BoundsGuard) {
                    continue;
                }
                if (n.data_inputs.empty() || n.data_inputs[0] != alias) {
                    continue;
                }
                int64_t key = 0;
                if (n.kind == NodeKind::Store && access_key_of(n, key) &&
                    n.data_inputs.size() >= 2) {
                    stores.push_back({nid, key});
                    anchor_block = built.block_of[nid];
                } else if (n.kind == NodeKind::Load &&
                           access_key_of(n, key)) {
                    loads.push_back(nid);
                    anchor_block = built.block_of[nid];
                } else if (n.kind != NodeKind::FrameState) {
                    if (std::getenv("VORTEX_EA_TRACE") != nullptr) {
                        std::fprintf(stderr,
                                     "[ea] alloc %u: escaping user %u "
                                     "kind=%d\n",
                                     id, nid, static_cast<int>(n.kind));
                        std::fprintf(stderr, "[ea]   user kind hex=%x\n",
                                     n.aux);
                    }
                    esc = true;
                }
            }
        }
        if (esc) {
            if (std::getenv("VORTEX_EA_TRACE") != nullptr) {
                std::fprintf(stderr, "[ea] alloc %u: escapes (access shape)\n",
                             id);
            }
            continue;
        }
        // Single-block scope: every access in one block (M3 boundary).
        for (const auto& [sid, key] : stores) {
            if (built.block_of[sid] != anchor_block) esc = true;
        }
        for (const NodeId lid : loads) {
            if (built.block_of[lid] != anchor_block) esc = true;
        }
        if (esc) {
            if (std::getenv("VORTEX_EA_TRACE") != nullptr) {
                std::fprintf(stderr, "[ea] alloc %u: escapes (multi-block)\n",
                             id);
            }
            continue;
        }

        // Exactly one store per field key (conservative ordering rule).
        std::unordered_map<int64_t, NodeId> single;
        for (const auto& [sid, key] : stores) {
            auto it = single.find(key);
            if (it != single.end()) {
                esc = true;
                break;
            }
            single.emplace(key, sid);
        }
        if (esc) {
            if (trace) {
                std::fprintf(stderr, "[ea] alloc %u: escapes (multi-store)\n",
                             id);
            }
            continue;
        }
        // Every load's key must have its single store BEFORE it on the
        // block's effect chain (program order, not id order); otherwise
        // the load sees undefined.
        std::unordered_map<NodeId, uint32_t> order;
        chain_order(g, built, anchor_block, order);
        for (const NodeId lid : loads) {
            int64_t key = 0;
            access_key_of(g.node(lid), key);
            const auto it = single.find(key);
            if (it == single.end() ||
                order[it->second] >= order[lid]) {
                if (trace) {
                    std::fprintf(stderr,
                                 "[ea] alloc %u: load %u key=%lld has "
                                 "store=%s store_rank=%u load_rank=%u "
                                 "store_block=%u load_block=%u\n",
                                 id, lid, (long long)key,
                                 it != nullptr ? "late" : "none",
                                 it != nullptr ? order[it->second] : 0u,
                                 order[lid],
                                 it != nullptr ? built.block_of[it->second]
                                               : 0u,
                                 built.block_of[lid]);
                }
                esc = true;
                break;
            }
        }
        if (esc) {
            if (trace) {
                std::fprintf(stderr, "[ea] alloc %u: escapes (load order)\n",
                             id);
            }
            continue;
        }

        if (trace) {
            std::fprintf(stderr,
                         "[ea] alloc %u: alias-set=%zu stores=%zu "
                         "loads=%zu anchor=%u\n",
                         id, aliases.size(), stores.size(), loads.size(),
                         anchor_block);
            for (const auto& [sid, key] : stores) {
                std::fprintf(stderr, "[ea]   store %u key=%lld kind=%d\n",
                             sid, (long long)key,
                             static_cast<int>(g.node(sid).aux));
            }
            for (const NodeId lid : loads) {
                std::fprintf(stderr, "[ea]   load %u key=%lld kind=%d\n",
                             lid, (long long)g.node(lid).const_value,
                             static_cast<int>(g.node(lid).aux));
            }
        }
        // FrameState rematerialization license: every FS referencing the
        // allocation (or an alias) must ALSO reference every field value.
        bool fs_ok = true;
        for (uint32_t fid = 0; fid < g.node_count() && fs_ok; ++fid) {
            const Node& fs = g.node(fid);
            if (fs.dead || fs.kind != NodeKind::FrameState) continue;
            bool references = false;
            for (const NodeId in : fs.data_inputs) {
                for (const NodeId alias : aliases) {
                    if (in == alias) references = true;
                }
            }
            if (!references) continue;
            for (const auto& [sid, key] : stores) {
                const NodeId value = g.node(sid).data_inputs[1];
                if (g.node(value).kind == NodeKind::Const) {
                    continue;  // constants rematerialize from the record
                }
                bool present = false;
                for (const NodeId in : fs.data_inputs) {
                    if (in == value) present = true;
                }
                if (!present) {
                    fs_ok = false;
                    break;
                }
            }
        }
        if (!fs_ok) {
            if (std::getenv("VORTEX_EA_TRACE") != nullptr) {
                std::fprintf(stderr, "[ea] alloc %u: escapes (fs license)\n",
                             id);
            }
            continue;
        }

        // Scalar replacement (phase 2): loads take the stored value;
        // stores and the allocation die; FS references become the
        // undefined word (the runtime rebuilds the object from the record).
        BuiltGraph::ScalarReplacement info;
        info.klass_token = alloc.aux;
        // Mixed key domains (Field tokens vs RawOffset byte offsets) inside
        // one alias set have no sound slot derivation -> escape.
        for (const auto& [sid, key] : stores) {
            if (key < 0) {
                if (trace) {
                    std::fprintf(stderr,
                                 "[ea] alloc %u: escapes (mixed key "
                                 "domains)\n", id);
                }
                esc = true;
                break;
            }
        }
        if (esc) continue;
        const auto* klass =
            static_cast<const vortex::Klass*>((*built.klass_addrs)[alloc.aux]);
        info.field_count = klass->field_count();
        info.fields.reserve(stores.size());
        for (const auto& [sid, key] : stores) {
            // RawOffset keys ARE byte offsets: slot = (key - header)/8.
            const uint32_t slot = static_cast<uint32_t>(
                (key - static_cast<int64_t>(sizeof(ObjectHeader))) / 8);
            info.fields.push_back({key, g.node(sid).data_inputs[1], slot});
        }
        std::sort(info.fields.begin(), info.fields.end(),
                  [](const auto& a, const auto& b) { return a.slot < b.slot; });
        for (const NodeId lid : loads) {
            int64_t key = 0;
            access_key_of(g.node(lid), key);
            for (const auto& [sid, skey] : stores) {
                if (skey == key) {
                    j2::replace_data_uses(g, lid, g.node(sid).data_inputs[1]);
                    break;
                }
            }
        }
        // The TRUE undefined word (0x7 — an odd, non-Smi sentinel; 0x6
        // would decode as Smi(3), a type confusion on the deopt path).
        const NodeId undef = g.add_const(
            static_cast<int64_t>(vortex::TaggedValue::undefined().raw()));
        info.undef_id = undef;
        for (uint32_t fid = 0; fid < g.node_count(); ++fid) {
            Node& fs = g.node(fid);
            if (fs.dead || fs.kind != NodeKind::FrameState) continue;
            for (NodeId& in : fs.data_inputs) {
                for (const NodeId alias : aliases) {
                    if (in == alias) in = undef;
                }
            }
        }
        for (const auto& [sid, key] : stores) {
            g.node(sid).dead = true;
            j2::replace_effect_input(g, sid, g.node(sid).effect_in);
        }
        for (const NodeId lid : loads) g.node(lid).dead = true;
        for (const NodeId alias : aliases) {
            if (alias != id) {
                j2::replace_data_uses(g, alias, undef);
                g.node(alias).dead = true;
            }
        }
        g.node(id).dead = true;
        // Dual-key the lookup: the FrameStates now reference `undef` (the
        // alloc id no longer appears in them), so build_deopt_records must
        // find the info through BOTH identities.
        built.scalar_replacements.emplace(
            id, BuiltGraph::ScalarReplacement{info});
        built.scalar_replacements.emplace(
            undef, BuiltGraph::ScalarReplacement{std::move(info)});
        ++replaced;
        if (trace) {
            std::fprintf(stderr, "[ea] alloc %u: SCALAR REPLACED\n", id);
        }
    }
    if (replaced != 0) {
        g.eliminate_dead_nodes();
        j2::sync_built(g, built);
    }
    return replaced;
}

}  // namespace vortex::j3
