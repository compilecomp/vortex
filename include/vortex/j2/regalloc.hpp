// J2 linear-scan register allocation (docs/tier-j2.md stage 18).
//
// SSA linear scan over the block LAYOUT: positions number (block, node-in-
// block) pairs; live intervals derive from SSA defs plus per-block liveness
// (live-in/live-out over the CFG), and intervals crossing a safepoint
// (call/allocation) prefer callee-saved registers or spill because the
// runtime helpers clobber the caller-saved set.
//
// Register budget (the J1 execution ABI pins r15 = context, r14 = ret
// pointer, rsp/rbp are the frame): 12 GP (9 caller-saved + rbx/r12/r13) and
// 8 xmm for raw f64 payloads. Spill slots live at [rbp - slot*8 - 24].
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vortex/ir/son_graph.hpp"
#include "vortex/j2/graph_builder.hpp"

namespace vortex::j2 {

/// General-purpose registers available to the allocator (the ABI-reserved
/// r14/r15/rsp/rbp are excluded by construction).
enum class PhysReg : uint8_t {
    RAX = 0, RCX, RDX, RSI, RDI, R8, R9, R10, R11,  // caller-saved
    RBX, R12, R13,                                   // callee-saved
    kGpCount,
    kFirstCallerSaved = RAX,
    kLastCallerSaved = R11,
    kFirstCalleeSaved = RBX,
};

enum class PhysXmm : uint8_t { XMM0 = 0, kXmmCount = 8 };

struct Location {
    enum class Kind : uint8_t { None, Gp, Xmm, Spill };
    Kind kind = Kind::None;
    uint8_t reg = 0;      // PhysReg / PhysXmm ordinal
    int32_t slot = 0;     // frame slot index (spill)
    bool valid() const noexcept { return kind != Kind::None; }
};

struct Allocation {
    std::vector<Location> location;  // per NodeId
    /// Home slots: register-resident values live across a safepoint get a
    /// spill slot the emitter stores into before the call (the GC map marks
    /// these slots — Rule 78: every reference across a safepoint is mapped).
    std::vector<int32_t> home_slot;  // per NodeId; -1 = none
    uint32_t spill_slots = 0;
    uint32_t gp_spill_slots = 0;
    uint32_t xmm_spill_slots = 0;
    /// Node ids of the safepoint positions (Call/Allocate/Safepoint), in
    /// layout order — the emitter stores live refs at these points.
    std::vector<uint32_t> safepoint_positions;
    /// Interval set snapshot (layout order) for the emitter's live-value
    /// queries (GC maps, safepoint stores).
    struct IntervalView {
        uint32_t vreg;
        uint32_t start;
        uint32_t end;
        bool xmm;
    };
    std::vector<IntervalView> intervals;
    /// Live node ids at a linear position (empty when none).
    std::vector<ir::NodeId> live_at(uint32_t pos) const;
};

/// Reference-shaped type test for GC mapping (Rule 78): Unknown counts as a
/// reference (the lattice may refine), proven primitives do not.
bool is_ref_type(ir::JType t);

/// Runs linear scan over the graph in `layout` block order. Deterministic
/// (Rule 56): intervals sort by (start, id), free-register choice is
/// lowest-ordinal-first.
///
// ---- emission plan (definition-before-use scheduling) -----------------------
//
// Node ids do NOT imply program order once inlining has spliced callee nodes
// behind caller consumers. The plan fixes the per-block emission order
// (Kahn's algorithm over data + effect dependencies, min-id tie-break —
// Rule 56 determinism) and the tagged-arith fusion decisions the emitter
// applies. The SAME plan drives the register allocator's position numbering
// through emission_positions, so the interval model and the emitted program
// point of every value can never disagree (a value read after a call is
// guaranteed to have an interval that crosses the call's safepoint).
struct EmissionPlan {
    /// Fused tagged-arith chain: the Add materializes at one site and the
    /// OverflowGuard is its deopt + store anchor. Only `guard` is populated
    /// by the current scan (the builder's typed-arith lowering carries no
    /// separate Untag/Tag satellites for fused chains — the operands are
    /// the raw tagged words); the other fields stay for future lowering.
    struct Fused {
        ir::NodeId untag_a = ir::kNoNode;
        ir::NodeId untag_b = ir::kNoNode;
        ir::NodeId guard = ir::kNoNode;
        ir::NodeId tag = ir::kNoNode;
    };
    std::unordered_map<ir::NodeId, Fused> fused_arith;
    std::unordered_set<ir::NodeId> fused_skip;
    /// Per-block emission order (emittable nodes only, fused satellites
    /// excluded — they materialize at their Add). Indexed by block id.
    std::vector<std::vector<ir::NodeId>> block_order;
    /// A data dependency cycle inside one block (impossible for a correct
    /// SSA graph). The plan still carries a total id-order fallback, but
    /// compile_j2 refuses it — silent consumers-before-producers emission
    /// is a miscompile, not a degradation (Rule 76).
    bool cyclic = false;
};

EmissionPlan plan_emission(const ir::Graph& graph, const BuiltGraph& built);

/// Canonical position numbering shared by the allocator and the emitter:
/// per layout block, rank = defs-at-entry (Const/Parameter/Phi/Start/
/// Region/Loop/End, id order), then the plan's emission order, then
/// block-end readers (FrameState/Return/If, id order — their reads extend
/// operand liveness to the block end). Nodes the plan does not schedule
/// rank by id (the pre-inlining behavior).
std::vector<uint32_t> emission_positions(
    const ir::Graph& graph, const BuiltGraph& built,
    const std::vector<uint32_t>& layout, const EmissionPlan& plan,
    uint32_t* stride_out = nullptr);

/// Linear scan over the block LAYOUT. `reserve_gp_mask` keeps PhysReg i out
/// of the pool entirely: the J2 emitter drives fixed transients through RAX
/// (universal scratch), RCX (index/argument staging), RDX, RSI and RDI
/// (helper ABI staging, store-value scratch, klass compares) — a live value
/// homed in any of them would be clobbered mid-sequence. `reserve_xmm_temps`
/// excludes XMM0+XMM1 — the fixed f64 transients (payload loads,
/// binop/compare inputs, cvtsi2sd). `plan` (from plan_emission) must be the
/// same object the emitter schedules by.
Allocation allocate_registers(
    const ir::Graph& graph, const BuiltGraph& built,
    const std::vector<uint32_t>& layout, uint32_t reserved_gp_mask = 0,
    const std::vector<ir::NodeId>* forced_spill = nullptr,
    bool reserve_xmm_temps = false, const EmissionPlan* plan = nullptr);

}  // namespace vortex::j2
