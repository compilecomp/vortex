#include "vortex/j2/regalloc.hpp"

#include <algorithm>

namespace vortex::j2 {
namespace {

using ir::AccessKind;
using ir::JType;
using ir::kNoNode;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;

constexpr uint32_t kGpCount = static_cast<uint32_t>(PhysReg::kGpCount);
constexpr uint32_t kXmmCount = static_cast<uint32_t>(PhysXmm::kXmmCount);
constexpr uint32_t kCalleeSavedFirst =
    static_cast<uint32_t>(PhysReg::kFirstCalleeSaved);

bool uses_xmm(const Node& n) {
    switch (n.kind) {
    case NodeKind::FAdd: case NodeKind::FSub: case NodeKind::FMul:
    case NodeKind::FDiv: case NodeKind::FNeg: case NodeKind::IToF:
        return true;
    case NodeKind::Load:
        // Raw double payloads ride the xmm file (the boxed double was
        // untangled by its BoxedF64 guard; the payload bits themselves are
        // not GC references).
        return static_cast<AccessKind>(n.aux) == AccessKind::DoublePayload;
    default:
        return false;
    }
}

struct Interval {
    NodeId vreg = 0;
    uint32_t start = 0;
    uint32_t end = 0;
    bool crosses_safepoint = false;
    bool xmm = false;
};

struct Allocator {
    const ir::Graph& g;
    const BuiltGraph& built;
    const std::vector<uint32_t>& layout;
    const std::vector<NodeId>* forced_spill = nullptr;
    const EmissionPlan* plan = nullptr;
    uint32_t reserved_gp_mask = 0;
    bool reserve_xmm_temps = false;
    bool is_forced(NodeId id) const {
        if (forced_spill == nullptr) return false;
        return std::find(forced_spill->begin(), forced_spill->end(), id) !=
               forced_spill->end();
    }
    std::vector<uint32_t> position_of;   // per layout block: block position
    std::vector<uint32_t> block_pos;     // per block id: layout index
    std::vector<uint32_t> block_first;   // per block id: first position
    std::vector<uint32_t> block_last;    // per block id: last position
    std::vector<std::vector<uint8_t>> live_in;
    std::vector<std::vector<uint8_t>> live_out;
    std::vector<uint32_t> safepoint_positions;  // sorted
    /// Per node: the LAST position that reads the node as a data input.
    /// Includes FrameState consumers — a vreg referenced only by a deopt
    /// snapshot is live up to the snapshot (Rule 39: the stub materializes
    /// the exact frame from these locations).
    std::vector<uint32_t> last_use;
    std::vector<Interval> intervals;
    uint32_t stride = 1;

    Allocator(const ir::Graph& graph, const BuiltGraph& b,
              const std::vector<uint32_t>& l)
        : g(graph), built(b), layout(l) {}

    bool is_safepoint(const Node& n) const {
        // Calls, allocations and polls are safepoints. Div/Rem lower to the
        // generic-binop C++ helper (SysV caller-saved clobber, same class
        // as invoke) — values live across them need callee-saved/spill.
        // Generic (helper-implemented) field accesses are TOO: the backend
        // lowers them to context-indirect calls into T0 (which may
        // allocate), so no value may live across them in a caller-saved
        // register (Rule 78).
        if (n.kind == NodeKind::Call || n.kind == NodeKind::Allocate ||
            n.kind == NodeKind::Safepoint || n.kind == NodeKind::Div ||
            n.kind == NodeKind::Rem) {
            return true;
        }
        if (n.kind == NodeKind::Load || n.kind == NodeKind::Store) {
            return static_cast<AccessKind>(n.aux) == AccessKind::Field;
        }
        return false;
    }

    void number() {
        // Positions come from the SHARED emission numbering (the same plan
        // the emitter schedules by) so intervals reflect the true program
        // order after inlining re-ordered definitions behind consumers. The
        // stride comes back so the block geometry below matches exactly.
        EmissionPlan fallback;
        const EmissionPlan& p = plan != nullptr ? *plan : fallback;
        uint32_t s = 1;
        position_of = emission_positions(g, built, layout, p, &s);
        stride = s;
        // Block geometry for the cross-block interval extension below.
        block_pos.assign(built.blocks.size(), UINT32_MAX);
        block_first.assign(built.blocks.size(), UINT32_MAX);
        block_last.assign(built.blocks.size(), 0);
        for (size_t li = 0; li < layout.size(); ++li) {
            const uint32_t b = layout[li];
            block_pos[b] = static_cast<uint32_t>(li);
            block_first[b] = static_cast<uint32_t>(li * stride);
            block_last[b] = static_cast<uint32_t>(li * stride + stride - 1);
        }
    }

    /// Consumers define the end of every interval: a def-only interval would
    /// free the register between a definition deep in a block and its later
    /// use (and would kill values kept alive solely by a FrameState). One
    /// pass over all data edges fixes both; block liveness below still
    /// extends loop-carried ranges across backedges.
    void compute_last_use() {
        last_use.assign(g.node_count(), 0);
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            const Node& n = g.node(id);
            if (n.dead) continue;
            const uint32_t pos = position_of[id];
            if (pos == UINT32_MAX) continue;
            for (const NodeId in : n.data_inputs) {
                if (in >= g.node_count()) continue;
                if (g.node(in).dead) continue;
                if (last_use[in] < pos) last_use[in] = pos;
            }
        }
    }

    void liveness() {
        const size_t nb = built.blocks.size();
        live_in.assign(nb, {});
        live_out.assign(nb, {});
        std::vector<std::vector<uint8_t>> use(nb), def(nb);
        for (size_t b = 0; b < nb; ++b) {
            use[b].assign(g.node_count(), 0);
            def[b].assign(g.node_count(), 0);
            live_in[b].assign(g.node_count(), 0);
            live_out[b].assign(g.node_count(), 0);
        }
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            const Node& n = g.node(id);
            if (n.dead) continue;
            const uint32_t b = id < built.block_of.size()
                                   ? built.block_of[id]
                                   : UINT32_MAX;
            if (b >= nb) continue;
            def[b][id] = 1;
            for (const NodeId in : n.data_inputs) {
                if (in >= g.node_count()) continue;
                if (!def[b][in]) use[b][in] = 1;
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t bi = nb; bi-- > 0;) {
                const uint32_t b = static_cast<uint32_t>(bi);
                for (const uint32_t s : built.blocks[b].succs) {
                    if (s >= nb) continue;
                    for (uint32_t id = 0; id < g.node_count(); ++id) {
                        if (live_out[b][id] < live_in[s][id]) {
                            live_out[b][id] = 1;
                            changed = true;
                        }
                    }
                }
                for (uint32_t id = 0; id < g.node_count(); ++id) {
                    const uint8_t in = use[b][id] ||
                                       (live_out[b][id] && !def[b][id]);
                    if (live_in[b][id] < in) {
                        live_in[b][id] = in;
                        changed = true;
                    }
                }
            }
        }
    }

    void collect_safepoints() {
        safepoint_positions.clear();
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            const Node& n = g.node(id);
            if (n.dead) continue;
            if (is_safepoint(n) && position_of[id] != UINT32_MAX) {
                safepoint_positions.push_back(position_of[id]);
            }
        }
        std::sort(safepoint_positions.begin(), safepoint_positions.end());
    }

    void build_intervals() {
        collect_safepoints();
        intervals.clear();
        intervals.reserve(g.node_count());
        // Predecessor lists for the phi-interval extension below (edge
        // copies execute in the pred's terminator, so a phi's register must
        // not be handed out to a value whose interval dies inside the pred).
        std::vector<std::vector<uint32_t>> preds(built.blocks.size());
        for (size_t b = 0; b < built.blocks.size(); ++b) {
            for (const uint32_t s : built.blocks[b].succs) {
                if (s < preds.size()) preds[s].push_back(static_cast<uint32_t>(b));
            }
        }
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            const Node& n = g.node(id);
            if (n.dead) continue;
            if (n.kind == NodeKind::Const) continue;  // rematerialized
            if (n.kind == NodeKind::Start ||
                n.kind == NodeKind::FrameState ||
                n.kind == NodeKind::Region || n.kind == NodeKind::End ||
                n.kind == NodeKind::If || n.kind == NodeKind::Loop) {
                continue;  // control/meta nodes occupy no register
            }
            const uint32_t def_pos = position_of[id];
            if (def_pos == UINT32_MAX) continue;
            Interval iv;
            iv.vreg = id;
            iv.start = def_pos;
            iv.end = std::max(def_pos, last_use[id]);
            iv.xmm = uses_xmm(n);
            // Extend to every block where the value is live.
            for (size_t b = 0; b < built.blocks.size(); ++b) {
                if (block_pos[b] == UINT32_MAX) continue;
                if (live_in[b][id] || live_out[b][id]) {
                    iv.start = std::min(iv.start, block_first[b]);
                    iv.end = std::max(iv.end, block_last[b]);
                }
            }
            // Phi edge copies: the value is WRITTEN in each predecessor's
            // terminator. Extend the interval to cover every pred block so
            // the scan never assigns the phi a register a pred-side value
            // (e.g. a flag-fused compare input) dies in — the copies run
            // before the branch direction is known.
            if (n.kind == NodeKind::Phi) {
                const uint32_t phib = built.block_of[id];
                if (phib < preds.size()) {
                    for (const uint32_t p : preds[phib]) {
                        if (block_first[p] != UINT32_MAX) {
                            iv.start = std::min(iv.start, block_first[p]);
                            iv.end = std::max(iv.end, block_last[p]);
                        }
                    }
                }
            }
            // Safepoint crossings (sorted positions, binary probe).
            iv.crosses_safepoint =
                std::any_of(safepoint_positions.begin(),
                            safepoint_positions.end(), [&](uint32_t p) {
                                return p >= iv.start && p <= iv.end;
                            });
            intervals.push_back(iv);
        }
        std::sort(intervals.begin(), intervals.end(),
                  [](const Interval& a, const Interval& b) {
                      if (a.start != b.start) return a.start < b.start;
                      return a.vreg < b.vreg;
                  });
    }

    Allocation scan() {
        Allocation alloc;
        alloc.location.assign(g.node_count(), Location{});
        alloc.safepoint_positions.clear();
        // Collect safepoint positions (layout order).
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            if (g.node(id).dead) continue;
            if (is_safepoint(g.node(id)) &&
                position_of[id] != UINT32_MAX) {
                alloc.safepoint_positions.push_back(position_of[id]);
            }
        }
        std::sort(alloc.safepoint_positions.begin(),
                  alloc.safepoint_positions.end());

        std::vector<int8_t> gp_free(kGpCount, 1);
        for (uint32_t r = 0; r < kGpCount; ++r) {
            if (reserved_gp_mask & (1u << r)) gp_free[r] = 0;
        }
        std::vector<int8_t> xmm_free(kXmmCount, 1);
        if (reserve_xmm_temps) {
            // The backend's fixed f64 transients (see the header contract):
            // XMM0 (payload-load scratch, binop lhs, cvtsi2sd) and XMM1
            // (binop rhs, neg zero operand). No live value may sit here.
            xmm_free[static_cast<uint32_t>(PhysXmm::XMM0)] = 0;
            xmm_free[static_cast<uint32_t>(PhysXmm::XMM0) + 1] = 0;
        }
        struct Active {
            Interval iv;
            uint8_t reg;
            bool xmm;
        };
        std::vector<Active> active;
        const auto retire = [&](uint32_t pos) {
            for (size_t i = 0; i < active.size();) {
                if (active[i].iv.end < pos) {
                    if (active[i].xmm) {
                        xmm_free[active[i].reg] = 1;
                    } else {
                        gp_free[active[i].reg] = 1;
                    }
                    active[i] = active.back();
                    active.pop_back();
                } else {
                    ++i;
                }
            }
        };
        const auto force_spill = [&](Interval& iv) {
            Location loc;
            loc.kind = Location::Kind::Spill;
            // ONE unified slot space: gp/xmm counters are per-class STATS,
            // but slot_disp addresses a single stack area — two counters
            // would give a GP slot 0 and an XMM slot 0 the same frame
            // displacement and silently alias the two values.
            loc.slot = static_cast<int32_t>(alloc.spill_slots++);
            if (iv.xmm) {
                ++alloc.xmm_spill_slots;
            } else {
                ++alloc.gp_spill_slots;
            }
            alloc.location[iv.vreg] = loc;
        };
        for (Interval& iv : intervals) {
            retire(iv.start);
            if (is_forced(iv.vreg)) {
                force_spill(iv);
                continue;
            }
            // Prefer caller-saved; intervals crossing safepoints need
            // callee-saved (the helpers clobber the caller-saved set).
            Location loc;
            bool placed = false;
            if (!iv.xmm) {
                uint32_t first = 0, last = kGpCount;
                if (iv.crosses_safepoint) {
                    first = kCalleeSavedFirst;
                }
                for (uint32_t r = first; r < last; ++r) {
                    if (gp_free[r]) {
                        gp_free[r] = 0;
                        loc.kind = Location::Kind::Gp;
                        loc.reg = static_cast<uint8_t>(r);
                        active.push_back({iv, static_cast<uint8_t>(r), false});
                        placed = true;
                        break;
                    }
                }
            } else if (iv.crosses_safepoint) {
                // SysV has no callee-saved xmm: the runtime helpers clobber
                // every xmm register, so a payload live across a call or
                // allocation must live in memory (the helpers' inline TLAB
                // bump is the common path; this is the rare spill shape).
                // force_spill already assigned the location — continue, do
                // NOT fall through to the final else (it would clobber the
                // Spill entry with a default Location{} and leave the value
                // with no location at all).
                force_spill(iv);
                continue;
            } else {
                for (uint32_t r = 0; r < kXmmCount; ++r) {
                    if (xmm_free[r]) {
                        xmm_free[r] = 0;
                        loc.kind = Location::Kind::Xmm;
                        loc.reg = static_cast<uint8_t>(r);
                        active.push_back({iv, static_cast<uint8_t>(r), true});
                        placed = true;
                        break;
                    }
                }
            }
            if (!placed) force_spill(iv);
            else alloc.location[iv.vreg] = loc;
        }
        alloc.spill_slots = alloc.gp_spill_slots + alloc.xmm_spill_slots;
        return alloc;
    }
};

}  // namespace

// ---- shared emission numbering ----------------------------------------------

std::vector<uint32_t> emission_positions(
    const ir::Graph& g, const BuiltGraph& built,
    const std::vector<uint32_t>& layout, const EmissionPlan& plan,
    uint32_t* stride_out) {
    const uint32_t n = g.node_count();
    // Per-block node counts fix the position stride.
    std::vector<uint32_t> count(built.blocks.size(), 0);
    for (uint32_t id = 0; id < n; ++id) {
        const Node& nd = g.node(id);
        if (nd.dead) continue;
        const uint32_t b =
            id < built.block_of.size() ? built.block_of[id] : UINT32_MAX;
        if (b < count.size()) ++count[b];
    }
    uint32_t stride = 1;
    for (const uint32_t c : count) stride = std::max(stride, c + 1);
    if (stride_out != nullptr) *stride_out = stride;

    std::vector<uint32_t> block_first(built.blocks.size(), UINT32_MAX);
    for (size_t li = 0; li < layout.size(); ++li) {
        block_first[layout[li]] = static_cast<uint32_t>(li * stride);
    }

    auto entry_def = [](NodeKind k) {
        switch (k) {
        case NodeKind::Const: case NodeKind::Parameter: case NodeKind::Phi:
        case NodeKind::Start: case NodeKind::Region: case NodeKind::Loop:
        case NodeKind::End:
            return true;
        default:
            return false;
        }
    };
    auto end_reader = [](NodeKind k) {
        return k == NodeKind::FrameState || k == NodeKind::Return ||
               k == NodeKind::If;
    };

    std::vector<uint32_t> pos(n, UINT32_MAX);
    for (size_t b = 0; b < built.blocks.size(); ++b) {
        if (block_first[b] == UINT32_MAX) continue;  // not in the layout
        uint32_t rank = 0;
        const auto place = [&](NodeId id) {
            if (id >= n || g.node(id).dead) return;
            const uint32_t nb = id < built.block_of.size()
                                    ? built.block_of[id]
                                    : UINT32_MAX;
            if (nb != b || pos[id] != UINT32_MAX) return;
            pos[id] = block_first[b] + rank++;
        };
        // 1. defs-at-entry (id order): parameters, constants, phis.
        for (uint32_t id = 0; id < n; ++id) {
            if (entry_def(g.node(id).kind)) place(id);
        }
        // 2. the plan's emission order, then any leftover emittable node in
        //    id order (covers plans built without a schedule for this block).
        if (b < plan.block_order.size()) {
            for (const NodeId id : plan.block_order[b]) place(id);
        }
        for (uint32_t id = 0; id < n; ++id) {
            const Node& nd = g.node(id);
            if (nd.dead || entry_def(nd.kind) || end_reader(nd.kind)) continue;
            if (plan.fused_skip.count(id) != 0) continue;  // placed at its Add
            place(id);
        }
        // 2b. fused-arith satellites materialize at their Add's site.
        for (const auto& [add, f] : plan.fused_arith) {
            if (add >= n || pos[add] == UINT32_MAX) continue;
            for (const NodeId sat : {f.guard, f.tag, f.untag_a, f.untag_b}) {
                if (sat == kNoNode || sat >= n) continue;
                if (g.node(sat).dead || pos[sat] != UINT32_MAX) continue;
                const uint32_t nb = sat < built.block_of.size()
                                        ? built.block_of[sat]
                                        : UINT32_MAX;
                if (nb != b) continue;
                pos[sat] = pos[add];
            }
        }
        // 3. block-end readers (id order): their reads extend operand
        //    liveness to the block end (terminators, deopt snapshots).
        for (uint32_t id = 0; id < n; ++id) {
            if (end_reader(g.node(id).kind)) place(id);
        }
    }
    return pos;
}

bool is_ref_type(JType t) {
    return t == JType::Ref || t == JType::BoxedF64 || t == JType::Unknown;
}

Allocation allocate_registers(const ir::Graph& graph, const BuiltGraph& built,
                              const std::vector<uint32_t>& layout,
                              uint32_t reserved_gp_mask,
                              const std::vector<NodeId>* forced_spill,
                              bool reserve_xmm_temps,
                              const EmissionPlan* plan) {
    Allocator a(graph, built, layout);
    a.reserved_gp_mask = reserved_gp_mask;
    a.reserve_xmm_temps = reserve_xmm_temps;
    a.plan = plan;
    a.number();
    a.compute_last_use();
    a.liveness();
    a.build_intervals();
    a.forced_spill = forced_spill;
    Allocation alloc = a.scan();
    // Home slots: register-resident values crossing a safepoint.
    alloc.home_slot.assign(graph.node_count(), -1);
    for (const Interval& iv : a.intervals) {
        if (!iv.crosses_safepoint || iv.xmm) continue;
        const Location& loc = alloc.location[iv.vreg];
        if (loc.kind != Location::Kind::Gp) continue;
        // Any live value needs a home (the emitter stores it before the
        // call); only ref-typed ones get GC-map bits later.
        alloc.home_slot[iv.vreg] = static_cast<int32_t>(alloc.spill_slots++);
    }
    // Interval snapshot for the emitter's live queries.
    for (const Interval& iv : a.intervals) {
        alloc.intervals.push_back({iv.vreg, iv.start, iv.end, iv.xmm});
    }
    return alloc;
}

std::vector<NodeId> Allocation::live_at(uint32_t pos) const {
    std::vector<NodeId> out;
    for (const IntervalView& iv : intervals) {
        if (iv.start <= pos && pos <= iv.end) out.push_back(iv.vreg);
    }
    return out;
}

}  // namespace vortex::j2
