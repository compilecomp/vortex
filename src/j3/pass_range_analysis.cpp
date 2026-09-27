// Stage 10: range analysis (the saturating Smi interval lattice).
// Split from src/j3/passes_j3.cpp — one pass per file.
#include "passes_internal.hpp"

#include <algorithm>

namespace vortex::j3 {

using namespace vortex::j2;
using ir::Graph;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;
using BuiltGraph = j2::BuiltGraph;

// ---- stage 10: range analysis ------------------------------------------------
//
// Smi interval lattice over the raw-domain nodes (Untag/Add/Sub/Mul/Const/
// Phi/Compare). Saturating at the Smi bounds; unknown = the full domain.
// (The Range struct itself lives in passes_internal.hpp — stage 36's
// bounds-check elimination consumes the same lattice.)

void ranges_of(Graph& g, const BuiltGraph& built,
               std::vector<Range>& out) {
    out.assign(g.node_count(), Range{});
    // Fixpoint (phis need it); bounded rounds, deterministic id order.
    for (int round = 0; round < 8; ++round) {
        bool changed = false;
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            const Node& n = g.node(id);
            if (n.dead) continue;
            Range r = out[id];
            switch (n.kind) {
            case NodeKind::Const:
                r.lo = r.hi = n.const_value;
                break;
            case NodeKind::Untag:
                if (!n.data_inputs.empty()) {
                    r = out[n.data_inputs[0]];  // untag preserves the range
                }
                break;
            case NodeKind::Add:
                if (n.data_inputs.size() == 2) {
                    const Range& a = out[n.data_inputs[0]];
                    const Range& b = out[n.data_inputs[1]];
                    // Saturating interval arithmetic (Rule 110: no signed
                    // overflow UB in the compiler itself).
                    int64_t lo = 0;
                    int64_t hi = 0;
                    if (__builtin_add_overflow(a.lo, b.lo, &lo) ||
                        lo < kSmiMin) {
                        lo = kSmiMin;
                    }
                    if (__builtin_add_overflow(a.hi, b.hi, &hi) ||
                        hi > kSmiMax) {
                        hi = kSmiMax;
                    }
                    r.lo = lo;
                    r.hi = hi;
                }
                break;
            case NodeKind::Sub:
                if (n.data_inputs.size() == 2) {
                    const Range& a = out[n.data_inputs[0]];
                    const Range& b = out[n.data_inputs[1]];
                    int64_t lo = 0;
                    int64_t hi = 0;
                    if (__builtin_sub_overflow(a.lo, b.hi, &lo) ||
                        lo < kSmiMin) {
                        lo = kSmiMin;
                    }
                    if (__builtin_sub_overflow(a.hi, b.lo, &hi) ||
                        hi > kSmiMax) {
                        hi = kSmiMax;
                    }
                    r.lo = lo;
                    r.hi = hi;
                }
                break;
            case NodeKind::Compare:
                r.lo = 0;
                r.hi = 1;
                break;
            case NodeKind::Phi:
                if (!n.data_inputs.empty()) {
                    r.lo = kSmiMax;
                    r.hi = kSmiMin;
                    for (const NodeId in : n.data_inputs) {
                        if (in == id || in == ir::kNoNode) continue;
                        r.lo = std::min(r.lo, out[in].lo);
                        r.hi = std::max(r.hi, out[in].hi);
                    }
                    if (r.lo > r.hi) r = Range{};
                }
                break;
            default:
                break;
            }
            if (r.lo != out[id].lo || r.hi != out[id].hi) {
                out[id] = r;
                changed = true;
            }
        }
        (void)built;
        if (!changed) break;
    }
}

}  // namespace vortex::j3
