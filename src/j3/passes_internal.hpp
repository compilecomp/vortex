// Internal cross-TU declarations of the J3 pass pipeline. The shared
// analysis types (the Smi range lattice, the access-key domain tag) live
// here with the per-pass entry points; every named pass is one
// translation unit (src/j3/pass_*.cpp) and the 60-stage driver stays in
// src/j3/passes_j3.cpp. The J3 pipeline reuses the J2 helpers so both
// tiers share one graph-mutation discipline (Rule 19: one IR, one set of
// invariants).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../j2/passes_internal.hpp"
#include "vortex/ir/ciog.hpp"
#include "vortex/ir/escape_summary.hpp"

namespace vortex::j3 {

// The Smi domain bounds (the int63 payload range, T0-verbatim).
inline constexpr int64_t kSmiMin =
    static_cast<int64_t>(0xFFFFFFFFFFFFFFFFull / 4 + 1);
inline constexpr int64_t kSmiMax =
    static_cast<int64_t>(0xFFFFFFFFFFFFFFFFull / 4);

/// The (base, key) alias key of a field access node: true for Field and
/// RawOffset accesses, with the key tagged by domain (Field keys are
/// module-wide TOKENS, RawOffset keys are BYTE OFFSETS — the two spaces
/// are not comparable and EA consumers must never mix them).
inline bool access_key_of(const ir::Node& n, int64_t& key) {
    const auto kind = static_cast<ir::AccessKind>(n.aux);
    if (kind != ir::AccessKind::Field && kind != ir::AccessKind::RawOffset) {
        return false;
    }
    key = n.const_value;
    if (kind == ir::AccessKind::Field) key = -1 - key;  // tag the domain
    return true;
}

/// Smi interval lattice value (saturating; unknown = the full domain).
struct Range {
    int64_t lo = kSmiMin;
    int64_t hi = kSmiMax;
    bool known() const { return lo != kSmiMin || hi != kSmiMax; }
};

// ---- the named passes (one TU each, src/j3/pass_*.cpp) ---------------------

// Stage 7: redundant load elimination + store-to-load forwarding.
uint32_t redundant_load_elim(ir::Graph& g, j2::BuiltGraph& built);

// Stage 10: range analysis (feeds stage 36).
void ranges_of(ir::Graph& g, const j2::BuiltGraph& built,
               std::vector<Range>& out);

// Stage 36: bounds-check elimination (range-driven).
uint32_t eliminate_bounds_checks(ir::Graph& g, j2::BuiltGraph& built,
                                 const std::vector<Range>& ranges);

// Stages 11/26/41/42: guard redundancy by dominance.
uint32_t eliminate_dominated_guards(ir::Graph& g, j2::BuiltGraph& built);

// Stages 12 + 16: escape analysis + scalar replacement over the merged
// graph (chain_order is local to the pass TU).
uint32_t scalar_replace(ir::Graph& g, j2::BuiltGraph& built);

// Stages 30 + 31: loop identification + LICM.
uint32_t licm(ir::Graph& g, j2::BuiltGraph& built);

// Stage 19: deferred field initialization.
uint32_t deferred_field_init(ir::Graph& g, j2::BuiltGraph& built);

// Stage 20: CIOG construction (the ir/ciog.hpp overlay records).
uint32_t build_ciog(ir::Graph& g, j2::BuiltGraph& built,
                    ir::CiogOverlay& ciog);

// Stage 29: interprocedural EA — the escape summary of THIS body.
ir::EscapeSummary summarize(const ir::Graph& g, const j2::BuiltGraph& built,
                            uint64_t graph_hash);

// Effect-chain rank of every live effectful node in one block: nodes
// earlier on the chain get smaller ranks (the chain is the program-order
// truth after splices — Rule 55). Defined in pass_scalar_replacement.cpp;
// shared with the chain-walking passes (RLE, deferred field init).
void chain_order(const ir::Graph& g, const j2::BuiltGraph& built,
                 uint32_t block, std::unordered_map<ir::NodeId, uint32_t>& rank);

}  // namespace vortex::j3
