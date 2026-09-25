// J2 backend: instruction selection, peephole, code emission, deopt and OSR
// (docs/tier-j2.md stages 19-21 + section 5). The emitted code executes
// against the J1 ABI (J1Context; helpers via context indirect calls, all in
// rel32 range of the shared code reservation).
//
// Emission contracts (T0-verbatim observable semantics, Rule 18):
//   - typed smi arith: payload-domain ops (sar 1, op, range check) with the
//     OverflowGuard doing the smi-range check + re-tag; hardware `jo` pins
//     Mul's int64 wrap; deopt on failure (T0 raises the identical error
//     after the deopt);
//   - canonical/generic forms: the shared canonical-binop helper;
//   - F64 ops: klass-guarded payload loads, IEEE arithmetic, fresh box per
//     result (allocation is observable — never folded);
//   - field/array access: tag/class guards, inline fast paths after IC
//     specialization, card-marking barriers on reference stores;
//   - every guard carries a complete FrameState (Rule 42) whose failure
//     materializes the frame registers into the deopt window and traps into
//     the runtime for a state-exact T0 resume (Rules 39/40).
//
// Backend register policy: RAX is RESERVED from the allocator and serves as
// the universal scratch (guard untangling, payload staging, deopt-window
// fills, call argument staging, phi cycle-breaking) — no live value ever
// sits in it. RCX is additionally reserved when the graph contains variable
// shifts (CL is the x86 shift-count register). Safepoint-crossing values
// live in callee-saved registers or spill slots; xmm payloads crossing a
// safepoint are force-spilled (SysV has no callee-saved xmm).
//
// CEM-26: this file is @warm per-method compile work (PERF-006 class — see
// docs/cem26.md); the GENERATED code's contracts are the emission comments
// above. The deopt hook (j2_deopt_hook) is the one @hot runtime entry.
#include "vortex/j2/fast_jit.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "vortex/codegen/x64/assembler.hpp"
#include "vortex/j2/graph_builder.hpp"
#include "vortex/j2/passes.hpp"
#include "vortex/j2/regalloc.hpp"
#include "vortex/runtime/object_model.hpp"

namespace vortex::j2 {
namespace {

// Debug trace switch (drivers/tests; read only on the rare deopt/compile
// trace paths) — flipped through the public set_j2_trace() below.
bool g_j2_trace = false;

using codegen::CodeBuffer;
using codegen::x64::Assembler;
using codegen::x64::CC_A;
using codegen::x64::CC_AE;
using codegen::x64::CC_B;
using codegen::x64::CC_BE;
using codegen::x64::CC_E;
using codegen::x64::CC_G;
using codegen::x64::CC_GE;
using codegen::x64::CC_L;
using codegen::x64::CC_LE;
using codegen::x64::CC_NE;
using codegen::x64::CC_NP;
using codegen::x64::CC_P;
using codegen::x64::Mem;
using codegen::x64::Reg;
using codegen::x64::Xmm;
using ir::AccessKind;
using j1::kUndefinedRawBits;

using j2::BuiltGraph;
using ir::CallShape;
using ir::CondCode;
using ir::GuardKind;
using ir::JType;
using ir::kNoNode;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;

// ---- context offsets (pinned by j1::J1Context static_asserts) ---------------
constexpr int32_t kCtxTlabTop = 0x08;
constexpr int32_t kCtxTlabEnd = 0x10;
constexpr int32_t kCtxCardBase = 0x18;
constexpr int32_t kCtxHeapBase = 0x20;
constexpr int32_t kCtxConstants = 0x28;
constexpr int32_t kCtxSafepointWord = 0x40;
constexpr int32_t kCtxJ2Deopt = 0x48;
constexpr int32_t kCtxAllocSlow = 0x80;
constexpr int32_t kCtxInvokeMethod = 0x98;
constexpr int32_t kCtxInvokeBuiltin = 0xA0;
constexpr int32_t kCtxGetFieldSlow = 0xA8;
constexpr int32_t kCtxSetFieldSlow = 0xB0;
constexpr int32_t kCtxGenericBinop = 0xC0;
constexpr int32_t kCtxLastError = 0xD8;

// Tagged-value word constants (support/tagged_value.hpp).
constexpr uint64_t kNullBits = 0x3;
constexpr uint64_t kUndefinedBits = 0x7;
constexpr uint64_t kFalseBits = 0xB;
constexpr uint64_t kTrueBits = 0xF;
constexpr int64_t kSmiMinPayload = static_cast<int64_t>(0xC000000000000000ull);
constexpr int64_t kSmiMaxPayload = 0x3FFFFFFFFFFFFFFFll;
constexpr uint32_t kTagMask = 0xF;
constexpr uint32_t kTagHeapBits = 0b0001;

// Object layout (runtime/object_model.hpp; TLAB strides rounded to 16).
constexpr int32_t kObjectHeaderBytes =
    static_cast<int32_t>(sizeof(ObjectHeader));
constexpr int32_t kBoxedDoubleBytes = kObjectHeaderBytes + 8;
constexpr int32_t kBoxedDoubleAllocBytes = (kBoxedDoubleBytes + 15) & ~15;
constexpr int32_t kArrayHeaderBytes =
    kObjectHeaderBytes + static_cast<int32_t>(ArrayObject::kLengthPadBytes);
constexpr int32_t kArrayLengthOffset = kObjectHeaderBytes;

// alloc_slow kinds (j1/context.hpp J1AllocSlowFn contract).
constexpr uint32_t kAllocObject = 0;
constexpr uint32_t kAllocArray = 1;
constexpr uint32_t kAllocDouble = 2;

// J2 array length ceiling (T0 InterpreterConfig::max_array_length default;
// named here and kept equal — the two engines must agree, Rule 18).
constexpr int32_t kJ2MaxArrayLength = 1'000'000;

// Deopt chain depth bound (the pipeline's inline_depth_cap + root, rounded
// up; a defensive named bound, not a tuning knob — Rule 72).
constexpr size_t kMaxDeoptFrames = 8;

// Error ids (the J1 surface — identical observable error wording).
constexpr uint32_t kErrDeopt = 1;
constexpr uint32_t kErrAddAny = 2;
constexpr uint32_t kErrSubAny = 6;
constexpr uint32_t kErrMulAny = 9;
constexpr uint32_t kErrDivS = 12;
constexpr uint32_t kErrDivZero = 14;
constexpr uint32_t kErrRemS = 15;
constexpr uint32_t kErrRemZero = 16;
constexpr uint32_t kErrNegI = 17;
constexpr uint32_t kErrBitop = 19;
constexpr uint32_t kErrCompare = 20;
constexpr uint32_t kErrCmpF64 = 22;
constexpr uint32_t kErrI64ToF64 = 23;
constexpr uint32_t kErrF64ToI64 = 24;
constexpr uint32_t kErrCallToken = 25;
constexpr uint32_t kErrCallBuiltin = 26;
constexpr uint32_t kErrNewArrayLen = 29;
constexpr uint32_t kErrAllocOOM = 30;
constexpr uint32_t kErrGetField = 31;
constexpr uint32_t kErrSetField = 32;
constexpr uint32_t kErrArrayLen = 33;
constexpr uint32_t kErrArrayGet = 34;
constexpr uint32_t kErrArraySet = 35;
constexpr uint32_t kErrCheckNull = 37;
constexpr uint32_t kErrCheckNonNull = 38;
constexpr uint32_t kErrCheckClass = 39;

uint32_t helper_error_id(uint32_t op) {
    switch (static_cast<ugb::Op>(op)) {
    case ugb::Op::SUB_ANY: return kErrSubAny;
    case ugb::Op::MUL_ANY: return kErrMulAny;
    case ugb::Op::EQ_ANY: case ugb::Op::COMPARE_ANY: case ugb::Op::EQ_REF:
    case ugb::Op::NE_REF:
        return kErrCompare;
    case ugb::Op::F64_TO_I64: return kErrF64ToI64;
    case ugb::Op::DIV_S_I64: return kErrDivS;
    case ugb::Op::REM_S_I64: return kErrRemS;
    default: return kErrAddAny;
    }
}

/// PhysReg ordinals (the allocator's) differ from the x86 Reg encodings —
/// the mapping is explicit so the two can never drift silently.
Reg gp(PhysReg r) {
    switch (r) {
    case PhysReg::RAX: return Reg::RAX;
    case PhysReg::RCX: return Reg::RCX;
    case PhysReg::RDX: return Reg::RDX;
    case PhysReg::RSI: return Reg::RSI;  // PhysReg 3 -> Reg 6
    case PhysReg::RDI: return Reg::RDI;  // PhysReg 4 -> Reg 7
    case PhysReg::R8: return Reg::R8;
    case PhysReg::R9: return Reg::R9;
    case PhysReg::R10: return Reg::R10;
    case PhysReg::R11: return Reg::R11;
    case PhysReg::RBX: return Reg::RBX;  // PhysReg 10 -> Reg 3
    case PhysReg::R12: return Reg::R12;
    case PhysReg::R13: return Reg::R13;
    default: return Reg::RAX;
    }
}
Xmm xmm_of(uint8_t r) { return static_cast<Xmm>(r); }
constexpr Reg kScratch = Reg::RAX;  // reserved from the allocator

static bool schedulable_kind(NodeKind k) {
    switch (k) {
    case NodeKind::Start: case NodeKind::Region: case NodeKind::Loop:
    case NodeKind::End: case NodeKind::If: case NodeKind::Return:
    case NodeKind::Const: case NodeKind::Parameter:
    case NodeKind::FrameState:
    case NodeKind::Phi:  // values arrive through edge copies
        return false;  // no code here (control handled at block end)
    default:
        return true;
    }
}

}  // namespace

// ---- emission plan (definition-before-use; see regalloc.hpp) ----------------
//
// The builder emits nodes in bytecode order, so id order is a topological
// order — UNTIL the inliner splices a callee's defining nodes behind the
// caller's consumers. Emission (and the register allocator's interval
// model) must follow the true dependency order, not ids. The plan is
// computed ONCE per compile and drives both the allocator numbering and
// the per-block emission walk.
EmissionPlan plan_emission(const ir::Graph& g, const BuiltGraph& built) {
    EmissionPlan plan;
    // ---- tagged-arith fusion scan (stage 20; docs/tier-j2.md 4) ----
    //
    // The builder lowers `Add.I32` into Untag(a) -> Untag(b) -> Add ->
    // OverflowGuard -> Tag. Both tagged words are EVEN, so their integer
    // add IS the tagged sum (bit 0 stays clear) and the int64 overflow
    // flag fires exactly when the int63 payload overflows — the chain
    // collapses into `add + jo` with no untag/retag.
    {
        std::unordered_map<NodeId, std::vector<NodeId>> users;
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            const Node& n = g.node(id);
            if (n.dead) continue;
            for (const NodeId in : n.data_inputs) {
                users[in].push_back(id);
            }
        }
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            const Node& n = g.node(id);
            if (n.dead || (n.kind != NodeKind::Add &&
                           n.kind != NodeKind::Sub &&
                           n.kind != NodeKind::Mul)) {
                continue;
            }
            if (n.data_inputs.size() != 2) continue;
            // The arith node's only data consumer must be its OverflowGuard.
            const auto& add_users = users[id];
            NodeId guard = kNoNode;
            bool exclusive = true;
            for (const NodeId u : add_users) {
                if (g.node(u).kind == NodeKind::FrameState) continue;
                if (g.node(u).kind == NodeKind::OverflowGuard &&
                    guard == kNoNode) {
                    guard = u;
                } else {
                    exclusive = false;
                    break;
                }
            }
            if (!exclusive || guard == kNoNode) continue;
            plan.fused_arith[id] = {kNoNode, kNoNode, guard, kNoNode};
            plan.fused_skip.insert(guard);
            if (g_j2_trace) {
                fprintf(stderr, "[j2-fuse] arith=%u guard=%u\n", id, guard);
            }
        }
    }
    const auto emission_site = [&plan](NodeId d) -> NodeId {
        for (const auto& [add, f] : plan.fused_arith) {
            if (f.guard == d || f.tag == d || f.untag_a == d ||
                f.untag_b == d) {
                return add;
            }
        }
        return d;
    };
    // ---- per-block topological order ----
    plan.block_order.assign(built.blocks.size(), {});
    for (uint32_t id = 0; id < g.node_count(); ++id) {
        const Node& n = g.node(id);
        if (n.dead || !schedulable_kind(n.kind)) continue;
        if (plan.fused_skip.count(id) != 0) continue;  // emitted by its Add
        if (built.block_of[id] == UINT32_MAX ||
            built.block_of[id] >= plan.block_order.size()) {
            continue;
        }
        plan.block_order[built.block_of[id]].push_back(id);
    }
    for (size_t b = 0; b < plan.block_order.size(); ++b) {
        std::vector<NodeId>& ids = plan.block_order[b];
        std::unordered_map<NodeId, uint32_t> indeg;
        std::unordered_map<NodeId, std::vector<NodeId>> users;
        for (const NodeId x : ids) indeg.emplace(x, 0);
        const auto add_edge = [&](NodeId from, NodeId to) {
            if (from == to) return;
            if (indeg.find(from) == indeg.end()) return;  // not in block
            users[from].push_back(to);
            ++indeg[to];
        };
        for (const NodeId x : ids) {
            const Node& n = g.node(x);
            for (const NodeId d : n.data_inputs) {
                if (d == kNoNode) continue;
                const Node& dn = g.node(d);
                if (dn.dead || !schedulable_kind(dn.kind)) continue;
                add_edge(emission_site(d), x);
            }
            // Effect order is side-effect order (the chain the builder and
            // the inliner wired); it must survive scheduling.
            if (n.effect_in != kNoNode) {
                const Node& en = g.node(n.effect_in);
                if (!en.dead && schedulable_kind(en.kind)) {
                    add_edge(emission_site(n.effect_in), x);
                }
            }
        }
        // Kahn: pick the smallest ready id each round (O(n^2) scan is fine
        // at J2 node counts and keeps the order deterministic — Rule 56).
        // Dep-free nodes keep id order, so graphs whose ids already satisfy
        // definition-before-use schedule identically to the old id walk.
        std::vector<NodeId> order;
        order.reserve(ids.size());
        bool progress = true;
        while (order.size() < ids.size() && progress) {
            progress = false;
            for (const NodeId x : ids) {
                const auto it = indeg.find(x);
                if (it == indeg.end() || it->second != 0) continue;
                order.push_back(x);
                it->second = UINT32_MAX;  // scheduled
                for (const NodeId u : users[x]) --indeg[u];
                progress = true;
            }
        }
        if (order.size() < ids.size()) {
            // A same-block data cycle cannot come out of a correct SSA
            // builder/inliner. Keep a total id-order fallback (the plan
            // stays inspectable) but flag it — compile_j2 refuses the
            // compile instead of silently emitting consumers before
            // producers (loud failure, Rule 76; same discipline as
            // fix_branch_abs on out-of-rel32 targets).
            plan.cyclic = true;
            std::vector<bool> done(g.node_count(), false);
            for (const NodeId x : order) done[x] = true;
            for (const NodeId x : ids) {
                if (!done[x]) order.push_back(x);
            }
        }
        ids.swap(order);
    }
    return plan;
}

namespace {  // emitter internals (internal linkage)

class Emitter {
public:
    Emitter(const ir::Graph& g, const BuiltGraph& built,
            const std::vector<uint32_t>& layout, const Allocation& alloc,
            const J2Job& job,
            std::shared_ptr<std::vector<DeoptRecord>> records,
            const EmissionPlan& plan)
        : g_(g), built_(built), layout_(layout), alloc_(alloc), job_(job),
          plan_(plan), records_(std::move(records)), asm_(buf_) {}

    // ---- frame geometry -------------------------------------------------------
    /// Slot 0 lives at [rbp + kSpillBase]. The prologue saves SIX registers
    /// (rbp, r15, r14, r13, r12, rbx = 48 bytes below rbp), so the spill
    /// base must clear the save area — a J1-inherited -24 here would alias
    /// slot 0 with the saved r13.
    static constexpr int32_t kSpillBase = -48;  // slot 0 at [rbp-48]
    /// Bytes below rbp the save area occupies (|kSpillBase|) — every frame
    /// reservation adds this so window writes never go below rsp.
    static constexpr int32_t kSaveAreaBytes = 48;
    /// Stack-alignment pad: six prologue pushes leave rsp = 8 mod 16, so the
    /// locals reservation adds one 8-byte pad to restore call-site alignment
    /// (both the entry and the OSR stubs; the epilogue releases pad + locals).
    static constexpr int32_t kFrameAlignPad = 8;

    int32_t slot_disp(int32_t slot) const { return kSpillBase - slot * 8; }

    support::Result<J2Code> emit() {
        J2Code out;
        out.method_id = job_.method_id;

        compute_positions();
        build_deopt_records();
        if (auto r = size_windows(); !r) return std::unexpected(r.error());

        // ---- prologue ------------------------------------------------------------
        // Saves rbx/r12/r13 (the allocator hands them out) next to the ABI
        // pair r15/r14. Six pushes leave rsp = 8 mod 16, so the locals
        // reservation adds 8 more than the 16-aligned locals size and every
        // call site stays ABI-aligned (the helper-callee contract, Rule 103).
        asm_.push_reg(Reg::RBP);
        asm_.mov_reg_reg(Reg::RBP, Reg::RSP);
        asm_.push_reg(Reg::R15);
        asm_.push_reg(Reg::R14);
        asm_.push_reg(Reg::R13);
        asm_.push_reg(Reg::R12);
        asm_.push_reg(Reg::RBX);
        const size_t sub_site = asm_.current_offset() + 3;
        asm_.sub_reg_imm32(Reg::RSP, 0);
        fixups_.push_back({FixupKind::Frame, sub_site, 0, 0});
        asm_.mov_reg_reg(Reg::R15, Reg::RDI);  // ctx
        asm_.mov_reg_reg(Reg::R14, Reg::RCX);  // ret

        // Entry: copy the argument window into the parameter locations.
        for (uint32_t v = 0; v < built_.register_count; ++v) {
            const NodeId param = entry_param_for_vreg(v);
            if (param == kNoNode) continue;
            const Location& loc = alloc_.location[param];
            if (v < built_.argc) {
                Mem src{Reg::RSI, Reg::RSP, 0, static_cast<int32_t>(8 * v)};
                store_mem_to_loc(loc, src);
            } else {
                // T0 fresh frames hold undefined beyond argc.
                store_const_to_loc(loc, kUndefinedBits);
            }
        }

        // ---- body -------------------------------------------------------------------
        for (size_t li = 0; li < layout_.size(); ++li) {
            const uint32_t b = layout_[li];
            layout_pos_[b] = li;
            block_offsets_[b] = buf_.size();
            if (auto r = emit_block(b); !r) return std::unexpected(r.error());
        }

        // ---- epilogue (shared tail) ----------------------------------------------------
        epilogue_offset_ = buf_.size();
        asm_.add_reg_imm32(Reg::RSP, locals_bytes_ + 8);
        asm_.pop_reg(Reg::RBX);
        asm_.pop_reg(Reg::R12);
        asm_.pop_reg(Reg::R13);
        asm_.pop_reg(Reg::R14);
        asm_.pop_reg(Reg::R15);
        asm_.pop_reg(Reg::RBP);
        asm_.ret();

        // ---- deopt stubs (out of line, one per guard record) ---------------
        for (const GuardSite& site : guard_sites_) {
            const size_t stub = emit_deopt_stub(site.record_idx);
            fixups_.push_back(
                {FixupKind::OolStub, site.jcc_at, 0, stub});
        }

        // ---- OSR stubs -------------------------------------------------------
        if (auto r = emit_osr_stubs(); !r) return std::unexpected(r.error());

        // ---- error blocks (lazily created; may append Epilogue fixups) --
        for (const ErrorFixup& ef : error_fixups_) {
            const size_t target = error_block_for(ef.err_id);
            const int64_t disp =
                static_cast<int64_t>(target) -
                static_cast<int64_t>(ef.at + 4);
            if (disp > 0x7FFFFFFFll || disp < -0x80000000ll) {
                return support::fail(support::ErrorCode::InternalError,
                                     "J2: error branch out of rel32 range");
            }
            patch_imm32(ef.at, static_cast<int32_t>(disp));
        }
        // ---- backward edges (allocation zero loops) ----------------------
        for (const BackwardFixup& bf : backward_fixups_) {
            const int64_t disp = static_cast<int64_t>(bf.target) -
                                 static_cast<int64_t>(bf.at + 4);
            if (disp < -0x80000000ll) {
                return support::fail(support::ErrorCode::InternalError,
                                     "J2: backward branch out of rel32");
            }
            patch_imm32(bf.at, static_cast<int32_t>(disp));
        }
        // ---- structural fixups ---------------------------------------------
        for (const Fixup& fx : fixups_) {
            int64_t target = 0;
            switch (fx.kind) {
            case FixupKind::Frame:
                patch_imm32(fx.at, locals_bytes_ + kFrameAlignPad);
                continue;
            case FixupKind::Block:
                target = block_offsets_.at(fx.target_block);
                break;
            case FixupKind::OolStub:
                target = fx.target_abs;
                break;
            case FixupKind::Epilogue:
                target = epilogue_offset_;
                break;
            }
            const int64_t disp = target - static_cast<int64_t>(fx.at + 4);
            if (disp > 0x7FFFFFFFll || disp < -0x80000000ll) {
                return support::fail(support::ErrorCode::InternalError,
                                     "J2: branch out of rel32 range");
            }
            patch_imm32(fx.at, static_cast<int32_t>(disp));
        }

        out.code = buf_.code();
        out.osr_entry_offset = osr_offset_;
        out.osr_pcs = osr_pcs_;
        out.gc_maps = serialize_gc_maps();
        out.deopt_records = serialize_deopt_records();
        out.records = records_;
        return out;
    }

private:
    enum class FixupKind : uint8_t { Frame, Block, OolStub, Epilogue };
    struct Fixup {
        FixupKind kind;
        size_t at;
        uint32_t target_block;
        uint64_t target_abs;
    };
    struct GuardSite {
        NodeId guard;
        size_t jcc_at;       // the failure jcc's rel32 payload position
        uint32_t record_idx;
    };

    // ---- positions (the regalloc numbering, mirrored for live queries) ------
    void compute_positions() {
        // The SHARED numbering (same plan the allocator numbered): positions
        // reflect the scheduled program order, not raw node ids — after
        // inlining, definitions can sit behind their consumers in id order
        // and every safepoint/GC-map query must agree with the intervals.
        position_ = emission_positions(g_, built_, layout_, plan_);
    }

    const Allocation::IntervalView* interval_of(NodeId v) const {
        for (const Allocation::IntervalView& iv : alloc_.intervals) {
            if (iv.vreg == v) return &iv;
        }
        return nullptr;
    }

    // ---- deopt records (pre-pass: stable addresses before any stub) ---------
    void build_deopt_records() {
        // Capacity first: reserve the exact count so push_back never
        // reallocations (the stubs embed record addresses, Rule 69).
        size_t record_count = 0;
        for (const NodeId id : built_.guards) {
            const Node& n = g_.node(id);
            if (n.dead) continue;  // guard-optimize killed it
            if (ir::is_guard(n.kind)) ++record_count;
        }
        for (uint32_t id = 0; id < g_.node_count(); ++id) {
            const Node& n = g_.node(id);
            if (n.dead) continue;
            if (n.kind == NodeKind::Load || n.kind == NodeKind::Store) {
                if (static_cast<AccessKind>(n.aux) ==
                    AccessKind::ArrayElement) {
                    ++record_count;
                }
            }
        }
        records_->reserve(record_count);

        const auto build_record = [&](NodeId fs_owner) -> uint32_t {
            const Node& owner = g_.node(fs_owner);
            if (owner.data_inputs.empty()) return UINT32_MAX;
            const NodeId fs0 = owner.data_inputs.back();
            if (fs0 == kNoNode ||
                g_.node(fs0).kind != NodeKind::FrameState) {
                return UINT32_MAX;
            }
            DeoptRecord rec;
            std::vector<NodeId> nodes;  // per frame: the FrameState node
            uint32_t base = 0;
            NodeId cur = fs0;
            BuiltGraph::InlineCallerInfo incoming;  // transition INTO `cur`
            bool first = true;
            while (cur != kNoNode && rec.frames.size() < kMaxDeoptFrames) {
                nodes.push_back(cur);
                const Node& f = g_.node(cur);
                DeoptFrame fr;
                fr.method_id = static_cast<uint32_t>(f.const_value);
                fr.resume_pc = f.aux;          // re-execute this pc
                fr.inject_dst = 0xFFFFFFFFu;   // no inject by default
                fr.vreg_base = base;
                fr.vreg_count = static_cast<uint32_t>(f.data_inputs.size());
                if (!first) {
                    // Entered via an inlined call: the transition recorded
                    // on the callee's FrameState says how the CALLER
                    // resumes (call-return pc, dst injected).
                    fr.resume_pc = incoming.return_pc;
                    fr.inject_dst = incoming.inject_dst;
                }
                rec.frames.push_back(fr);
                base += fr.vreg_count;
                const auto it = built_.inline_caller_frame.find(cur);
                if (it == built_.inline_caller_frame.end()) break;
                incoming = it->second;
                first = false;
                cur = it->second.frame_state;
            }
            records_->push_back(std::move(rec));
            frame_nodes_.push_back(std::move(nodes));
            return static_cast<uint32_t>(records_->size() - 1);
        };

        for (const NodeId id : built_.guards) {
            const Node& n = g_.node(id);
            if (n.dead) continue;  // guard-optimize killed it
            if (!ir::is_guard(n.kind)) continue;  // calls: GC-map points only
            record_index_of_[id] = build_record(id);
        }
        // Synthetic bounds checks: array accesses deopt into T0, which
        // re-executes the access with its own canonical behavior (Rule 30).
        for (uint32_t id = 0; id < g_.node_count(); ++id) {
            const Node& n = g_.node(id);
            if (n.dead) continue;
            if (n.kind != NodeKind::Load && n.kind != NodeKind::Store) {
                continue;
            }
            if (static_cast<AccessKind>(n.aux) != AccessKind::ArrayElement) {
                continue;
            }
            record_index_of_[id] = build_record(id);
        }
    }

    uint32_t record_of(NodeId n) const {
        const auto it = record_index_of_.find(n);
        return it == record_index_of_.end() ? UINT32_MAX : it->second;
    }

    support::Result<void> size_windows() {
        // Deepest FrameState chain fixes the deopt window; the widest call
        // fixes the argument window.
        uint32_t max_chain = 0;
        uint32_t max_frame_vregs = 0;
        for (const auto& [id, idx] : record_index_of_) {
            (void)idx;
            NodeId cur = g_.node(id).data_inputs.back();
            uint32_t chain = 0;
            while (cur != kNoNode && chain < kMaxDeoptFrames) {
                const Node& f = g_.node(cur);
                chain += static_cast<uint32_t>(f.data_inputs.size());
                max_frame_vregs = std::max(
                    max_frame_vregs,
                    static_cast<uint32_t>(f.data_inputs.size()));
                const auto it = built_.inline_caller_frame.find(cur);
                if (it == built_.inline_caller_frame.end()) break;
                cur = it->second.frame_state;
            }
            max_chain = std::max(max_chain, chain);
        }
        deopt_window_vregs_ = max_chain;
        uint32_t max_call_args = 0;
        for (uint32_t id = 0; id < g_.node_count(); ++id) {
            const Node& n = g_.node(id);
            if (n.dead || n.kind != NodeKind::Call) continue;
            // Every Call shape stages [args..., FrameState] — the aux word
            // discriminates the HELPER contract, not the input layout.
            const uint32_t args =
                static_cast<uint32_t>(n.data_inputs.size() - 1);
            max_call_args = std::max(max_call_args, args);
        }
        call_window_args_ = max_call_args + 1;  // + the out slot
        const int32_t spill = static_cast<int32_t>(alloc_.spill_slots);
        // The frame below rbp = |kSpillBase| (slot 0 lives at rbp-48) +
        // spill slots + deopt window + call window, rounded to 16. Skipping
        // the 48-byte save-area base here would push every window write
        // BELOW rsp — straight into the helpers' own frames.
        locals_bytes_ =
            (kSaveAreaBytes +
             (spill + static_cast<int32_t>(deopt_window_vregs_) +
              static_cast<int32_t>(call_window_args_)) *
                 8 +
             15) &
            ~15;
        deopt_window_disp_ =
            slot_disp(spill) - static_cast<int32_t>(deopt_window_vregs_) * 8;
        call_window_disp_ =
            deopt_window_disp_ - static_cast<int32_t>(call_window_args_) * 8;
        return support::ok();
    }

    // ---- location plumbing -----------------------------------------------------------
    NodeId entry_param_for_vreg(uint32_t v) const {
        // Parameters are vreg-indexed at build time; the builder creates
        // register_count Parameter nodes in vreg order.
        uint32_t seen = 0;
        for (uint32_t id = 0; id < g_.node_count(); ++id) {
            if (g_.node(id).dead) continue;
            if (g_.node(id).kind == NodeKind::Parameter) {
                if (seen == v) return id;
                ++seen;
            }
        }
        return kNoNode;
    }

    Reg loc_reg(const Location& loc) const {
        return gp(static_cast<PhysReg>(loc.reg));
    }

    void patch_imm32(size_t at, int32_t v) {
        std::memcpy(buf_.code().data() + at, &v, 4);
    }

    void store_mem_to_loc(const Location& loc, Mem src) {
        switch (loc.kind) {
        case Location::Kind::Spill:
            // mem -> mem through the reserved scratch (no live value in it).
            asm_.mov_reg_mem(kScratch, src);
            asm_.mov_mem_reg(Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)},
                             kScratch);
            break;
        case Location::Kind::Gp:
            asm_.mov_reg_mem(loc_reg(loc), src);
            break;
        default:
            break;  // xmm params do not exist (tagged entry ABI)
        }
    }

    void store_const_to_loc(const Location& loc, uint64_t bits) {
        const auto imm = static_cast<int64_t>(bits);
        switch (loc.kind) {
        case Location::Kind::Spill:
            if (imm >= -0x80000000ll && imm <= 0x7FFFFFFFll) {
                asm_.mov_mem_imm32sx(
                    Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)},
                    static_cast<int32_t>(imm));
            } else {
                asm_.mov_reg_imm64(kScratch, bits);
                asm_.mov_mem_reg(
                    Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)},
                    kScratch);
            }
            break;
        case Location::Kind::Gp:
            if (imm >= -0x80000000ll && imm <= 0x7FFFFFFFll) {
                asm_.mov_reg_imm32sx(loc_reg(loc),
                                     static_cast<int32_t>(imm));
            } else {
                asm_.mov_reg_imm64(loc_reg(loc), bits);
            }
            break;
        default:
            break;
        }
    }

    /// Const rematerialization: the value a vreg would hold. Smi-typed
    /// consts store their PAYLOAD in const_value (the builder tags them
    /// through the smi encoding); every other const stores the raw word.
    uint64_t const_word(const Node& c) const {
        if (built_.types[c.id] == JType::Smi) {
            return static_cast<uint64_t>(c.const_value) << 1;
        }
        return static_cast<uint64_t>(c.const_value);
    }

    bool is_const(NodeId v) const {
        if (v >= g_.node_count()) {
            fprintf(stderr, "[j2-bug] is_const with bad id %u\n", v);
        }
        return g_.node(v).kind == NodeKind::Const;
    }

    /// Loads `v`'s current value into `into` (rematerializing constants).
    support::Result<void> load_value(NodeId v, Reg into) {
        const Node& n = g_.node(v);
        if (n.kind == NodeKind::Const) {
            store_const_to_reg(into, const_word(n));
            return support::ok();
        }
        const Location& loc = alloc_.location[v];
        switch (loc.kind) {
        case Location::Kind::Gp:
            if (loc_reg(loc) != into) asm_.mov_reg_reg(into, loc_reg(loc));
            break;
        case Location::Kind::Spill:
            asm_.mov_reg_mem(into,
                             Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)});
            break;
        default:
            return support::fail(support::ErrorCode::InternalError,
                                 "J2: no location for value " +
                                     std::to_string(v));
        }
        return support::ok();
    }

    void store_const_to_reg(Reg into, uint64_t bits) {
        const auto imm = static_cast<int64_t>(bits);
        if (imm >= -0x80000000ll && imm <= 0x7FFFFFFFll) {
            asm_.mov_reg_imm32sx(into, static_cast<int32_t>(imm));
        } else {
            asm_.mov_reg_imm64(into, bits);
        }
    }

    void store_result(const Location& loc, Reg from) {
        switch (loc.kind) {
        case Location::Kind::Gp:
            if (loc_reg(loc) != from) asm_.mov_reg_reg(loc_reg(loc), from);
            break;
        case Location::Kind::Spill:
            asm_.mov_mem_reg(Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)},
                             from);
            break;
        default:
            break;
        }
    }

    /// Copies a value between locations (phi edge moves).
    void emit_move(const Location& dst, const Location& src) {
        switch (dst.kind) {
        case Location::Kind::Gp:
            switch (src.kind) {
            case Location::Kind::Gp:
                if (loc_reg(dst) != loc_reg(src)) {
                    asm_.mov_reg_reg(loc_reg(dst), loc_reg(src));
                }
                break;
            case Location::Kind::Spill:
                asm_.mov_reg_mem(
                    loc_reg(dst),
                    Mem{Reg::RBP, Reg::RSP, 0, slot_disp(src.slot)});
                break;
            default:
                break;
            }
            break;
        case Location::Kind::Spill:
            switch (src.kind) {
            case Location::Kind::Gp:
                asm_.mov_mem_reg(
                    Mem{Reg::RBP, Reg::RSP, 0, slot_disp(dst.slot)},
                    loc_reg(src));
                break;
            case Location::Kind::Spill:
                // mem -> mem through the reserved scratch.
                asm_.mov_reg_mem(
                    kScratch,
                    Mem{Reg::RBP, Reg::RSP, 0, slot_disp(src.slot)});
                asm_.mov_mem_reg(
                    Mem{Reg::RBP, Reg::RSP, 0, slot_disp(dst.slot)},
                    kScratch);
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }

    void emit_move_const(const Location& dst, const Node& c) {
        if (dst.kind == Location::Kind::Gp) {
            store_const_to_reg(loc_reg(dst), const_word(c));
        } else if (dst.kind == Location::Kind::Spill) {
            store_const_to_loc(dst, const_word(c));
        }
    }

    // ---- error / deopt exits ---------------------------------------------------
    void error_exit(uint32_t err_id) {
        asm_.mov_reg_imm32sx(Reg::RAX, static_cast<int32_t>(err_id));
        asm_.mov_mem_imm32(Mem{Reg::R15, Reg::RSP, 0, kCtxLastError},
                           static_cast<int32_t>(err_id));
        jmp_fixup(FixupKind::Epilogue);
    }

    /// Error exit for helpers that already returned the error id in rax and
    /// recorded last_error themselves (invoke_method/invoke_builtin).
    void error_exit_from_rax() { jmp_fixup(FixupKind::Epilogue); }

    /// Deopt on guard failure: the jcc target is patched to the guard's
    /// out-of-line stub (frame materialization + runtime hook).
    support::Result<void> guard_fail(NodeId guard, size_t jcc_at) {
        const uint32_t rec = record_of(guard);
        if (rec == UINT32_MAX) {
            return support::fail(support::ErrorCode::InternalError,
                                 "J2: guard without a deopt record");
        }
        guard_sites_.push_back({guard, jcc_at, rec});
        return support::ok();
    }

    // ---- block emission -----------------------------------------------------------------
    support::Result<void> emit_block(uint32_t b) {
        // Scheduled order (definition-before-use; see plan_emission).
        // Blocks with no scheduled nodes (pure control) still emit their
        // terminator below.
        if (b < plan_.block_order.size()) {
            for (const NodeId id : plan_.block_order[b]) {
                const Node& n = g_.node(id);
                if (n.dead) continue;
                // Tagged-arith fusion: the Untag/OverflowGuard/Tag satellites
                // of a fused Add/Sub are folded into the single tagged add
                // (see plan_emission).
                if (plan_.fused_skip.count(id) != 0) {
                    continue;
                }
                // Flag fusion: a smi Compare consumed by THIS block's If is
                // materialized by the branch itself (one compare, setcc + LEA)
                // — emitting it here as well would double the compare and add
                // a branch to every hot-loop iteration.
                if (n.kind == NodeKind::Compare) {
                    const Block& blk = built_.blocks[b];
                    if (blk.control_end != kNoNode) {
                        const Node& end = g_.node(blk.control_end);
                        if (end.kind == NodeKind::If && !end.dead &&
                            end.data_inputs.size() == 1 &&
                            end.data_inputs[0] == id &&
                            static_cast<CondCode>(n.aux) < CondCode::LtF) {
                            continue;
                        }
                    }
                }
                if (auto r = emit_node(id, n); !r) return r;
            }
        }
        // ---- block terminator ------------------------------------------------------------
        const Block& blk = built_.blocks[b];
        if (blk.control_end == kNoNode) {
            // Fall-through block (no terminator): the successor is reached
            // by falling through the layout order — its phi edge copies
            // still run, and an explicit jmp keeps the CFG honest when the
            // layout separates the two blocks.
            if (blk.succs.size() == 1) {
                emit_edge_copies(b, blk.succs[0]);
                const auto it = layout_pos_.find(b);
                if (it != layout_pos_.end() &&
                    it->second + 1 < layout_.size() &&
                    layout_[it->second + 1] != blk.succs[0]) {
                    jmp_fixup(FixupKind::Block, blk.succs[0]);
                }
            }
            return support::ok();
        }
        const Node& end = g_.node(blk.control_end);
        switch (end.kind) {
        case NodeKind::Return: {
            // Result -> *r14; rax = 0; exit.
            const NodeId val = end.data_inputs[0];
            store_result_to_ret(val);
            asm_.xor_reg_reg(Reg::RAX, Reg::RAX);
            jmp_fixup(FixupKind::Epilogue);
            break;
        }
        case NodeKind::End: {
            // Unconditional: branch to the (single) successor.
            if (blk.succs.size() == 1) {
                emit_edge_copies(b, blk.succs[0]);
                jmp_fixup(FixupKind::Block, blk.succs[0]);
            }
            break;
        }
        case NodeKind::If: {
            if (blk.succs.size() != 2) {
                return support::fail(support::ErrorCode::InternalError,
                                     "J2: conditional with wrong arity");
            }
            const Node& cond = g_.node(end.data_inputs[0]);
            if (auto r = emit_conditional_branch(blk, cond); !r) return r;
            break;
        }
        default:
            break;
        }
        return support::ok();
    }

    /// Parallel copies for the edge pred -> succ (phi inputs, indexed by
    /// the pred position in the successor's pred list).
    void emit_edge_copies(uint32_t pred, uint32_t succ) {
        const Block& sb = built_.blocks[succ];
        for (uint32_t id = 0; id < g_.node_count(); ++id) {
            const Node& n = g_.node(id);
            if (n.dead || n.kind != NodeKind::Phi) continue;
            if (built_.block_of[id] != succ) continue;
            uint32_t idx = UINT32_MAX;
            for (uint32_t i = 0; i < sb.preds.size(); ++i) {
                if (sb.preds[i] == pred) { idx = i; break; }
            }
            if (idx == UINT32_MAX || idx >= n.data_inputs.size()) continue;
            const NodeId src = n.data_inputs[idx];
            const Location& dst = alloc_.location[id];
            if (!dst.valid()) continue;
            if (src == kNoNode) continue;
            if (is_const(src)) {
                emit_move_const(dst, g_.node(src));
                continue;
            }
            const Location& sloc = alloc_.location[src];
            if (!sloc.valid()) continue;
            pending_moves_.push_back(EdgeMove{dst, sloc});
        }
        resolve_edge_moves();
    }

    struct EdgeMove {
        Location dst;
        Location src;
    };
    std::vector<EdgeMove> pending_moves_;

    /// Parallel-move resolution (Rule 56: deterministic — the first ready
    /// move in node order; cycles broken through the reserved scratch).
    void resolve_edge_moves() {
        auto same_loc = [](const Location& a, const Location& b) {
            return a.kind == b.kind && a.reg == b.reg && a.slot == b.slot;
        };
        while (!pending_moves_.empty()) {
            int chosen = -1;
            for (size_t i = 0; i < pending_moves_.size(); ++i) {
                bool dst_is_pending_src = false;
                for (size_t j = 0; j < pending_moves_.size(); ++j) {
                    if (j != i &&
                        same_loc(pending_moves_[j].src,
                                 pending_moves_[i].dst)) {
                        dst_is_pending_src = true;
                        break;
                    }
                }
                if (!dst_is_pending_src) { chosen = static_cast<int>(i); break; }
            }
            if (chosen < 0) {
                // Cycle: stash one source in the reserved scratch, route
                // the move through it, emit immediately (the scratch is
                // only live between the stash and this one use).
                EdgeMove& m = pending_moves_.front();
                switch (m.src.kind) {
                case Location::Kind::Gp:
                    asm_.mov_reg_reg(kScratch, loc_reg(m.src));
                    break;
                case Location::Kind::Spill:
                    asm_.mov_reg_mem(
                        kScratch,
                        Mem{Reg::RBP, Reg::RSP, 0, slot_disp(m.src.slot)});
                    break;
                default:
                    break;
                }
                Location via;
                via.kind = Location::Kind::Gp;
                via.reg = static_cast<uint8_t>(PhysReg::RAX);
                emit_move(m.dst, via);
                pending_moves_.erase(pending_moves_.begin());
                continue;
            }
            emit_move(pending_moves_[chosen].dst,
                      pending_moves_[chosen].src);
            pending_moves_.erase(pending_moves_.begin() + chosen);
        }
    }

    void store_result_to_ret(NodeId val) {
        // *r14 = value (the TaggedValue result slot of the J1 ABI).
        if (val >= g_.node_count()) {
            fprintf(stderr, "[j2-bug] store_result_to_ret bad val %u\n", val);
        }
        if (is_const(val)) {
            const Node& c = g_.node(val);
            asm_.mov_reg_imm32sx(kScratch,
                                 static_cast<int32_t>(const_word(c)));
            asm_.mov_mem_reg(Mem{Reg::R14, Reg::RSP, 0, 0}, kScratch);
            return;
        }
        const Location& loc = alloc_.location[val];
        switch (loc.kind) {
        case Location::Kind::Gp:
            asm_.mov_mem_reg(Mem{Reg::R14, Reg::RSP, 0, 0}, loc_reg(loc));
            break;
        case Location::Kind::Spill: {
            asm_.mov_reg_mem(
                kScratch, Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)});
            asm_.mov_mem_reg(Mem{Reg::R14, Reg::RSP, 0, 0}, kScratch);
            break;
        }
        default:
            break;
        }
    }

    void branch_to_block(const Block& blk, bool true_edge) {
        const uint32_t target =
            true_edge ? (blk.target_is_true_edge ? blk.succs[0]
                                                 : blk.succs[1])
                      : (blk.target_is_true_edge ? blk.succs[1]
                                                 : blk.succs[0]);
        jmp_fixup(FixupKind::Block, target);
    }

    void emit_edge_copies_for_branch(const Block& blk, bool true_edge) {
        const uint32_t succ =
            true_edge ? (blk.target_is_true_edge ? blk.succs[0]
                                                 : blk.succs[1])
                      : (blk.target_is_true_edge ? blk.succs[1]
                                                 : blk.succs[0]);
        emit_edge_copies(built_.block_of[blk.control_end], succ);
    }

    support::Result<void> emit_conditional_branch(const Block& blk,
                                                  const Node& cond) {
        if (g_j2_trace) {
            fprintf(stderr, "[j2-branch] cond kind=%d aux=%u inputs=%zu\n",
                    (int)cond.kind, cond.aux, cond.data_inputs.size());
        }
        // Conditions: Compare nodes (flag-producing) or smi truthiness.
        if (cond.kind == NodeKind::Compare && cond.data_inputs.size() == 2) {
            const bool f64 =
                static_cast<CondCode>(cond.aux) >= CondCode::LtF;
            if (f64) {
                return emit_f64_compare_branch(blk, cond);
            }
            return emit_smi_compare_branch(blk, cond);
        }
        // Truthiness (T0 TaggedValue::truthy semantics, Rule 18).
        // The condition is STAGED before any edge copies run: a copy's
        // target may reuse a register the condition value dies in (the
        // allocator frees it at this node), and reading the clobbered
        // register would invert the branch (Rule 78 discipline).
        const NodeId c = cond.id;
        const auto load = load_value(c, kScratch);
        if (!load) {
            // Unlocatable condition — refuse loudly (Rule 76); the caller
            // keeps the method on J1/T0.
            return support::fail(support::ErrorCode::InternalError,
                                 "J2: condition value has no location");
        }
        emit_edge_copies_for_branch(blk, /*true_edge=*/true);
        asm_.test_reg_imm8(kScratch, 1);
        const size_t smi = asm_.placeholder_jcc(CC_E);
        // Non-smi: falsey iff the word is false/null/undefined. Over the
        // valid encoding set, (w - 3) <= 8 unsigned covers exactly
        // {0x3, 0x7, 0xB} — true (0xF) and heap pointers fall outside.
        asm_.sub_reg_imm32(kScratch, 3);
        asm_.cmp_reg_imm32(kScratch, 8);
        const size_t falsey = asm_.placeholder_jcc(CC_BE);
        const size_t truthy_jmp = asm_.placeholder_jmp();
        // Smi path: truthy iff the whole word is nonzero (payload != 0).
        asm_.bind_placeholder(smi);
        asm_.test_reg_imm32(kScratch, -1);
        const size_t smi_zero = asm_.placeholder_jcc(CC_E);
        asm_.bind_placeholder(truthy_jmp);
        branch_to_block(blk, true);
        asm_.bind_placeholder(falsey);
        asm_.bind_placeholder(smi_zero);
        emit_edge_copies_for_branch(blk, /*true_edge=*/false);
        branch_to_block(blk, false);
        return support::ok();
    }

    support::Result<void> emit_smi_compare_branch(const Block& blk,
                                                  const Node& cond) {
        // Stage the FULL compare before any edge copies run — a true-edge
        // copy target may reuse a register the compare inputs die in, and
        // the copies execute before the branch direction is known (the
        // mov instructions leave the flags intact, so the order
        // compare -> copies -> setcc/jcc is exact).
        const NodeId a = cond.data_inputs[0];
        const NodeId b = cond.data_inputs[1];
        if (g_j2_trace) {
            const Location& al = alloc_.location[a];
            const Location& bl = alloc_.location[b];
            fprintf(stderr,
                    "[j2-cmpbr] cond=%u a=%u(kind=%d lk=%d reg=%u slot=%d) "
                    "b=%u(kind=%d lk=%d reg=%u slot=%d)\n",
                    cond.id, a, (int)g_.node(a).kind, (int)al.kind, al.reg,
                    al.slot, b, (int)g_.node(b).kind, (int)bl.kind, bl.reg,
                    bl.slot);
        }
        const auto la = load_value(a, kScratch);
        if (!la) {
            return support::fail(support::ErrorCode::InternalError,
                                 "J2: compare operand has no location");
        }
        const Location& bloc = alloc_.location[b];
        switch (bloc.kind) {
        case Location::Kind::Gp:
            asm_.cmp_reg_reg(kScratch, loc_reg(bloc));
            break;
        case Location::Kind::Spill:
            asm_.cmp_reg_mem(
                kScratch,
                Mem{Reg::RBP, Reg::RSP, 0, slot_disp(bloc.slot)});
            break;
        default: {
            // Const operand: immediate compare (tagged smi words compare
            // directly — payloads are doubled, order preserved).
            const int32_t imm =
                static_cast<int32_t>(const_word(g_.node(b)));
            asm_.cmp_reg_imm32(kScratch, imm);
            break;
        }
        }
        emit_edge_copies_for_branch(blk, /*true_edge=*/true);
        // ONE compare feeds BOTH the branch and the condition value. The
        // standalone value materialization would re-compare and double the
        // hot-loop branch cost. The setcc -> bool transform (c*4 +
        // kFalseBits, giving kFalseBits/kTrueBits) runs through LEA and
        // SETCC, which both LEAVE THE FLAGS INTACT — the branch JCC below
        // still reads the compare's flags. The materialized value keeps the
        // condition's location fresh for state-exact deopt (Rule 39).
        const Location& cloc = alloc_.location[cond.id];
        if (cloc.valid()) {
            asm_.setcc(cc_of(static_cast<CondCode>(cond.aux)), Reg::RCX);
            asm_.lea_reg_scaled_disp(Reg::RCX, Reg::RCX, 2,
                                     static_cast<int32_t>(kFalseBits));
            store_result(cloc, Reg::RCX);
        }
        const size_t jcc = asm_.placeholder_jcc(
            cc_of(static_cast<CondCode>(cond.aux)));
        branch_jcc_to_block(blk, jcc, true);
        emit_edge_copies_for_branch(blk, /*true_edge=*/false);
        branch_to_block(blk, false);
        return support::ok();
    }

    support::Result<void> emit_f64_compare_branch(const Block& blk,
                                                  const Node& cond) {
        // Stage the full compare before any edge copies run (see
        // emit_smi_compare_branch — same register-reuse hazard).
        const NodeId a = cond.data_inputs[0];
        const NodeId b = cond.data_inputs[1];
        const auto la = load_xmm_value(a, Xmm::XMM0);
        const auto lb = load_xmm_value(b, Xmm::XMM1);
        if (!la || !lb) {
            return support::fail(support::ErrorCode::InternalError,
                                 "J2: f64 compare operand has no location");
        }
        asm_.ucomisd(Xmm::XMM0, Xmm::XMM1);
        emit_edge_copies_for_branch(blk, /*true_edge=*/true);
        // Unordered -> false for every ordered predicate (NaN semantics,
        // Rule 110; J1 parity). The mov edge copies leave the flags (and
        // the ucomisd result) intact.
        const size_t jp = asm_.placeholder_jcc(CC_P);
        const size_t jcc = asm_.placeholder_jcc(ordered_cc(cond.aux));
        branch_jcc_to_block(blk, jcc, true);
        emit_edge_copies_for_branch(blk, /*true_edge=*/false);
        branch_to_block(blk, false);
        // jp target: the unordered-false path = the false block.
        const uint32_t false_block =
            blk.target_is_true_edge ? blk.succs[1] : blk.succs[0];
        fixups_.push_back({FixupKind::Block, jp, false_block, 0});
        return support::ok();
    }

    void branch_jcc_to_block(const Block& blk, size_t jcc_at, bool true_edge) {
        const uint32_t target =
            true_edge ? (blk.target_is_true_edge ? blk.succs[0]
                                                 : blk.succs[1])
                      : (blk.target_is_true_edge ? blk.succs[1]
                                                 : blk.succs[0]);
        fixups_.push_back({FixupKind::Block, jcc_at, target, 0});
    }

    support::Result<void> load_xmm_value(NodeId v, Xmm into) {
        const Node& n = g_.node(v);
        if (n.kind == NodeKind::Const) {
            // A constant payload: load the f64 bits through the scratch.
            asm_.mov_reg_imm64(kScratch,
                               static_cast<uint64_t>(n.const_value));
            asm_.movq_xmm_gpr(into, kScratch);
            return support::ok();
        }
        const Location& loc = alloc_.location[v];
        switch (loc.kind) {
        case Location::Kind::Xmm:
            if (xmm_of(loc.reg) != into) {
                asm_.movsd_xmm_xmm(into, xmm_of(loc.reg));
            }
            break;
        case Location::Kind::Spill:
            asm_.movsd_xmm_mem(
                into, Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)});
            break;
        case Location::Kind::Gp:
            asm_.movq_xmm_gpr(into, loc_reg(loc));
            break;
        default:
            return support::fail(support::ErrorCode::InternalError,
                                 "J2: no location for f64 value " +
                                     std::to_string(v));
        }
        return support::ok();
    }

    uint8_t cc_of(CondCode code) const {
        switch (code) {
        case CondCode::Eq: return CC_E;
        case CondCode::Ne: return CC_NE;
        case CondCode::LtS: return CC_L;
        case CondCode::LeS: return CC_LE;
        case CondCode::GtS: return CC_G;
        case CondCode::GeS: return CC_GE;
        default: return CC_E;  // f64 handled by the xmm path
        }
    }

    uint8_t ordered_cc(uint32_t aux) const {
        switch (static_cast<CondCode>(aux)) {
        case CondCode::Eq: return CC_E;
        case CondCode::LtF: return CC_B;
        case CondCode::LeF: return CC_BE;
        case CondCode::GtF: return CC_A;
        default: return CC_AE;
        }
    }

    void jmp_fixup(FixupKind kind, uint32_t block = 0) {
        const size_t at = asm_.placeholder_jmp();
        fixups_.push_back({kind, at, block, 0});
    }

    // ---- node emission (stages 19-21) -----------------------------------------
    support::Result<void> emit_node(NodeId id, const Node& n) {
        switch (n.kind) {
        // ---- payload-domain smi arithmetic (the OverflowGuard re-tags) --
        case NodeKind::Add: case NodeKind::Sub: case NodeKind::Mul:
            return emit_smi_arith(id, n);
        case NodeKind::OverflowGuard:
            return emit_overflow_guard(id, n);
        // ---- tagged-domain bitwise ops (commute with the tag bit) -------
        case NodeKind::And: case NodeKind::Or: case NodeKind::Xor:
            return emit_bitop(id, n);
        case NodeKind::Shift:
            return emit_shift(id, n);
        case NodeKind::Neg:
            return emit_neg_i64(id, n);
        case NodeKind::Div: case NodeKind::Rem:
            return emit_div_rem(id, n);
        // ---- raw f64 domain ---------------------------------------------
        case NodeKind::FAdd: case NodeKind::FSub: case NodeKind::FMul:
        case NodeKind::FDiv:
            return emit_f64_binop(id, n);
        case NodeKind::FNeg:
            return emit_f64_neg(id, n);
        case NodeKind::IToF:
            return emit_i64_to_f64(id, n);
        case NodeKind::Tag:
            return emit_tag(id, n, /*untag=*/false);
        case NodeKind::Untag:
            return emit_tag(id, n, /*untag=*/true);
        // ---- compares as values ------------------------------------------
        case NodeKind::Compare:
            return emit_compare_value(id, n);
        // ---- guards --------------------------------------------------------
        case NodeKind::TypeGuard:
            return emit_type_guard(id, n);
        case NodeKind::ClassGuard:
            return emit_class_guard(id, n);
        case NodeKind::BoundsGuard:
            return emit_bounds_guard(id, n);
        // ---- memory --------------------------------------------------------
        case NodeKind::Load:
            return emit_load(id, n);
        case NodeKind::Store:
            return emit_store(id, n);
        // ---- calls / allocation / polls (safepoints) ------------------------
        case NodeKind::Call:
            return emit_call(id, n);
        case NodeKind::Allocate:
            return emit_allocate(id, n);
        case NodeKind::Safepoint:
            return emit_safepoint_poll(id);
        default:
            return support::fail(
                support::ErrorCode::Unimplemented,
                "J2: no selection for node kind " +
                    std::to_string(static_cast<uint32_t>(n.kind)));
        }
    }

    // Typed smi arithmetic computes the int63 PAYLOAD (the OverflowGuard
    // range-checks and re-tags — see emit_overflow_guard).
    // Tagged-domain arithmetic fusion + the per-block emission schedule
    // live in plan_emission() (shared with the register allocator's
    // position numbering); this emitter only reads the plan.

    support::Result<void> emit_smi_arith(NodeId id, const Node& n) {
        const auto fit = plan_.fused_arith.find(id);
        if (fit != plan_.fused_arith.end()) {
            // Fused tagged-domain form: load the TAGGED operands (the
            // Untag nodes' own inputs), add/sub in place, `jo` to the
            // guard's deopt stub, and store the tagged result straight
            // into the re-tag's location.
            const EmissionPlan::Fused& f = fit->second;
            const Location& guard_loc = alloc_.location[f.guard];
            const auto la = load_value(n.data_inputs[0], kScratch);
            if (!la) return la;
            const auto lb = load_value(n.data_inputs[1], Reg::RCX);
            if (!lb) return lb;
            if (n.kind == NodeKind::Add || n.kind == NodeKind::Sub) {
                if (n.kind == NodeKind::Add) {
                    asm_.add_reg_reg(kScratch, Reg::RCX);
                } else {
                    asm_.sub_reg_reg(kScratch, Reg::RCX);
                }
                const size_t ovf = asm_.placeholder_jcc(codegen::x64::CC_O);
                if (auto r = guard_fail(f.guard, ovf); !r) return r;
                store_result(guard_loc, kScratch);
                return support::ok();
            }
            // Mul: the tagged operands must be untagged for imul, and the
            // int64 wrap check alone does not cover the int63 payload
            // window — the in-register roundtrip pins the range. Both
            // checks stay at the mul site; the guard's spill round-trip
            // disappears.
            const auto ua = stage_operand(n.data_inputs[0], id, kScratch,
                                          /*untag=*/true,
                                          /*into_reserved=*/true);
            if (!ua) return std::unexpected(ua.error());
            const auto ub = stage_operand(n.data_inputs[1], id, Reg::RCX,
                                          /*untag=*/true, rcx_reserved_);
            if (!ub) return std::unexpected(ub.error());
            asm_.imul_reg_reg(ua->reg, ub->reg);
            // Both failure paths deopt DIRECTLY at the fused guard (the
            // guard node is fused_skip — emit_overflow_guard never runs for
            // it, so a site pushed to mul_overflow_sites_ would never be
            // patched and overflow would fall through silently).
            const size_t wrap = asm_.placeholder_jcc(codegen::x64::CC_O);
            if (auto r = guard_fail(f.guard, wrap); !r) return r;
            // int63 range: (payload << 1) >> 1 == payload, checked on a COPY
            // in RCX — the product register must not move against itself
            // (a self-compare is always equal and never fires). RCX held
            // operand B's staged copy, dead since the imul.
            asm_.mov_reg_reg(Reg::RCX, ua->reg);
            asm_.shl_reg_imm8(Reg::RCX, 1);
            asm_.sar_reg_imm8(Reg::RCX, 1);
            asm_.cmp_reg_reg(Reg::RCX, ua->reg);
            const size_t rng = asm_.placeholder_jcc(codegen::x64::CC_NE);
            if (auto r = guard_fail(f.guard, rng); !r) return r;
            asm_.shl_reg_imm8(ua->reg, 1);  // re-tag the payload
            if (ub->pop_rdx) asm_.pop_reg(Reg::RDX);
            if (ua->pop_rdx) asm_.pop_reg(Reg::RDX);
            store_result(guard_loc, ua->reg);
            return support::ok();
        }
        const Location& loc = alloc_.location[id];
        // RAX is always reserved; RCX only when the graph shifts.
        auto la = stage_operand(n.data_inputs[0], id, kScratch,
                                /*untag=*/true, /*into_reserved=*/true);
        if (!la) return std::unexpected(la.error());
        auto lb = stage_operand(n.data_inputs[1], id, Reg::RCX,
                                /*untag=*/true, rcx_reserved_);
        if (!lb) return std::unexpected(lb.error());  // compile aborts: balance moot
        switch (n.kind) {
        case NodeKind::Add: asm_.add_reg_reg(la->reg, lb->reg); break;
        case NodeKind::Sub: asm_.sub_reg_reg(la->reg, lb->reg); break;
        case NodeKind::Mul: {
            asm_.imul_reg_reg(la->reg, lb->reg);
            // int64 wrap is unrepresentable in the payload domain: pin it
            // here — the guard's range check alone would miss wrapped
            // products that re-enter the int63 window (T0 traps, Rule 110).
            const size_t ovf = asm_.placeholder_jcc(codegen::x64::CC_O);
            mul_overflow_sites_.push_back({id, ovf});
            break;
        }
        default: break;
        }
        if (lb->pop_rdx) asm_.pop_reg(Reg::RDX);
        if (la->pop_rdx) asm_.pop_reg(Reg::RDX);
        store_result(loc, la->reg);
        return support::ok();
    }

    std::vector<std::pair<NodeId, size_t>> mul_overflow_sites_;

    support::Result<void> emit_overflow_guard(NodeId id, const Node& n) {
        const NodeId raw = n.data_inputs[0];
        const Location& loc = alloc_.location[id];
        // The check needs the INTACT original payload: stage a COPY in the
        // reserved scratch (in-place staging would destroy the value the
        // compare must read). Copy discipline, not cleverness — Rule 56.
        const auto lr = load_value(raw, kScratch);
        if (!lr) return lr;
        // Smi range check WITHOUT an imm64 scratch: the payload fits the
        // int63 smi window iff (payload << 1) >> 1 == payload. The compare
        // happens BEFORE the tagged store (the guard's location may alias
        // the raw value's register — reuse is legal because the raw dies
        // at this guard).
        asm_.shl_reg_imm8(kScratch, 1);
        asm_.sar_reg_imm8(kScratch, 1);
        if (is_const(raw)) {
            asm_.cmp_reg_imm32(
                kScratch,
                static_cast<int32_t>(
                    static_cast<int64_t>(g_.node(raw).const_value)));
        } else {
            const Location& rawloc = alloc_.location[raw];
            switch (rawloc.kind) {
            case Location::Kind::Spill:
                asm_.cmp_reg_mem(
                    kScratch,
                    Mem{Reg::RBP, Reg::RSP, 0, slot_disp(rawloc.slot)});
                break;
            case Location::Kind::Gp:
                asm_.cmp_reg_reg(kScratch, loc_reg(rawloc));
                break;
            default:
                return support::fail(support::ErrorCode::InternalError,
                                     "J2: overflow guard without raw value");
            }
        }
        const size_t jcc = asm_.placeholder_jcc(CC_NE);
        if (auto r = guard_fail(id, jcc); !r) return r;
        // A Mul's hardware-overflow jcc (emitted with the arithmetic) fails
        // into the same stub.
        for (size_t i = 0; i < mul_overflow_sites_.size();) {
            if (mul_overflow_sites_[i].first == raw) {
                if (auto r = guard_fail(id, mul_overflow_sites_[i].second);
                    !r) {
                    return r;
                }
                mul_overflow_sites_.erase(mul_overflow_sites_.begin() + i);
            } else {
                ++i;
            }
        }
        // Re-tag and publish the guard's proven value.
        asm_.shl_reg_imm8(kScratch, 1);
        store_result(loc, kScratch);
        return support::ok();
    }

    support::Result<void> emit_bitop(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        // AND/OR/XOR of tagged smi words commutes with the tag bit — the
        // tagged words go straight in (T0-verbatim, no payload round-trip).
        auto la = stage_operand(n.data_inputs[0], id, kScratch,
                                /*untag=*/false, /*into_reserved=*/true);
        if (!la) return std::unexpected(la.error());
        auto lb = stage_operand(n.data_inputs[1], id, Reg::RCX,
                                /*untag=*/false, rcx_reserved_);
        if (!lb) return std::unexpected(lb.error());
        switch (n.kind) {
        case NodeKind::And: asm_.and_reg_reg(la->reg, lb->reg); break;
        case NodeKind::Or: asm_.or_reg_reg(la->reg, lb->reg); break;
        case NodeKind::Xor: asm_.xor_reg_reg(la->reg, lb->reg); break;
        default: break;
        }
        if (lb->pop_rdx) asm_.pop_reg(Reg::RDX);
        if (la->pop_rdx) asm_.pop_reg(Reg::RDX);
        store_result(loc, la->reg);
        return support::ok();
    }

    // ---- shifts / neg / div-rem -----------------------------------------------
    support::Result<void> emit_shift(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        // Inputs are Untag nodes (payloads). T0: shift = b & 63; the count
        // rides CL, staged through RDX so the mask mutation never hits a
        // shared value's own register.
        auto la = stage_operand(n.data_inputs[0], id, kScratch,
                                /*untag=*/false, /*into_reserved=*/true);
        if (!la) return std::unexpected(la.error());
        auto lb = stage_operand(n.data_inputs[1], id, Reg::RDX,
                                /*untag=*/false, /*into_reserved=*/false);
        if (!lb) return std::unexpected(lb.error());
        asm_.mov_reg_reg(Reg::RCX, lb->reg);
        if (lb->pop_rdx) asm_.pop_reg(Reg::RDX);
        if (la->pop_rdx) asm_.pop_reg(Reg::RDX);
        asm_.and_reg_imm32(Reg::RCX, 63);
        if (g_j2_trace) {
            fprintf(stderr, "[j2-shift] node=%u aux=%u\n", id, n.aux);
        }
        switch (n.aux) {  // shift kind: 0 = SHL, 1 = SAR, 2 = SHR
        case 0: asm_.shl_reg_cl(la->reg); break;
        case 1: asm_.sar_reg_cl(la->reg); break;
        default: asm_.shr_reg_cl(la->reg); break;
        }
        if (la->pop_rdx) {}  // (already popped above)
        if (n.aux == 0) {
            // Rule 110 / ADR-005: SHL is the only bitwise form that can
            // leave the Smi range — T0 traps, never wraps, so J2 must trap
            // too (differential parity, Rule 18). The count register is
            // dead after the shift; it stages the unsigned-bounds probe:
            // payload + 2^62 in [0, 2^63) unsigned covers exactly
            // [smi_min, smi_max]. Flags survive the RDX pop.
            asm_.mov_reg_reg(Reg::RCX, la->reg);
            asm_.push_reg(Reg::RDX);
            asm_.mov_reg_imm64(Reg::RDX, 0x4000000000000000ull);  // 2^62
            asm_.add_reg_reg(Reg::RCX, Reg::RDX);
            asm_.mov_reg_imm64(Reg::RDX, 0x7FFFFFFFFFFFFFFFull);
            asm_.cmp_reg_reg(Reg::RCX, Reg::RDX);
            asm_.pop_reg(Reg::RDX);
            const size_t ovf = asm_.placeholder_jcc(codegen::x64::CC_A);
            error_fixups_.push_back({ovf, kErrBitop});
        }
        store_result(loc, la->reg);
        return support::ok();
    }

    support::Result<void> emit_neg_i64(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        const auto la = load_value(n.data_inputs[0], kScratch);
        if (!la) return la;
        asm_.sar_reg_imm8(kScratch, 1);
        // T0: NEG of smi_min overflows — raise there directly (the error
        // path never returns to the fast frame, so unbalanced scratch is
        // impossible; imm64 compare through a push/pop-saved RDX).
        asm_.push_reg(Reg::RDX);
        asm_.mov_reg_imm64(Reg::RDX,
                           static_cast<uint64_t>(TaggedValue::smi_min()));
        asm_.cmp_reg_reg(kScratch, Reg::RDX);
        asm_.pop_reg(Reg::RDX);
        const size_t ovf = asm_.placeholder_jcc(CC_E);
        error_fixups_.push_back({ovf, kErrNegI});
        asm_.neg_reg(kScratch);
        asm_.shl_reg_imm8(kScratch, 1);
        store_result(loc, kScratch);
        return support::ok();
    }

    support::Result<void> emit_div_rem(NodeId id, const Node& n) {
        // T0-verbatim trap semantics live in the shared generic-binop
        // helper (the same one J1 calls): zero check, smi_min/-1 overflow,
        // unsigned forms. The tagged words go in; a tagged smi comes back.
        const Location& loc = alloc_.location[id];
        const auto la = load_value(n.data_inputs[0], Reg::RDX);
        if (!la) return la;
        const auto lb = load_value(n.data_inputs[1], Reg::RCX);
        if (!lb) return lb;
        asm_.mov_reg_reg(Reg::RDI, Reg::R15);  // ctx
        asm_.mov_reg_imm32sx(Reg::RSI, static_cast<int32_t>(n.aux));
        asm_.call_mem(Mem{Reg::R15, Reg::RSP, 0, kCtxGenericBinop});
        asm_.cmp_reg_imm32(kScratch, static_cast<int32_t>(kUndefinedRawBits));
        const size_t err = asm_.placeholder_jcc(CC_E);
        error_fixups_.push_back({err, helper_error_id(n.aux)});
        store_result(loc, kScratch);
        return support::ok();
    }

    // ---- raw f64 domain ---------------------------------------------------------
    support::Result<void> emit_f64_binop(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        const auto la = load_xmm_value(n.data_inputs[0], Xmm::XMM0);
        if (!la) return la;
        const auto lb = load_xmm_value(n.data_inputs[1], Xmm::XMM1);
        if (!lb) return lb;
        switch (n.kind) {
        case NodeKind::FAdd: asm_.addsd(Xmm::XMM0, Xmm::XMM1); break;
        case NodeKind::FSub: asm_.subsd(Xmm::XMM0, Xmm::XMM1); break;
        case NodeKind::FMul: asm_.mulsd(Xmm::XMM0, Xmm::XMM1); break;
        default: asm_.divsd(Xmm::XMM0, Xmm::XMM1); break;  // IEEE div-zero
        }
        store_xmm_result(loc, Xmm::XMM0);
        return support::ok();
    }

    void store_xmm_result(const Location& loc, Xmm from) {
        switch (loc.kind) {
        case Location::Kind::Xmm:
            if (xmm_of(loc.reg) != from) {
                asm_.movsd_xmm_xmm(xmm_of(loc.reg), from);
            }
            break;
        case Location::Kind::Spill:
            asm_.movsd_mem_xmm(
                Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)}, from);
            break;
        default:
            break;
        }
    }

    support::Result<void> emit_f64_neg(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        const auto la = load_xmm_value(n.data_inputs[0], Xmm::XMM0);
        if (!la) return la;
        asm_.pxor_xmm_xmm(Xmm::XMM1, Xmm::XMM1);
        asm_.subsd(Xmm::XMM1, Xmm::XMM0);  // 0 - x (IEEE sign flip; J1 shape)
        store_xmm_result(loc, Xmm::XMM1);
        return support::ok();
    }

    support::Result<void> emit_i64_to_f64(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        const NodeId src = n.data_inputs[0];  // an Untag node (payload)
        const auto la = load_value(src, kScratch);
        if (!la) return la;
        asm_.cvtsi2sd(Xmm::XMM0, kScratch);
        store_xmm_result(loc, Xmm::XMM0);
        return support::ok();
    }

    support::Result<void> emit_tag(NodeId id, const Node& n, bool untag) {
        const Location& loc = alloc_.location[id];
        const auto la = load_value(n.data_inputs[0], kScratch);
        if (!la) return la;
        if (untag) {
            asm_.sar_reg_imm8(kScratch, 1);
        } else {
            asm_.shl_reg_imm8(kScratch, 1);
        }
        store_result(loc, kScratch);
        return support::ok();
    }

    // ---- compares as values -------------------------------------------------
    support::Result<void> emit_compare_value(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        const bool f64 = static_cast<CondCode>(n.aux) >= CondCode::LtF;
        if (f64) {
            const auto la = load_xmm_value(n.data_inputs[0], Xmm::XMM0);
            if (!la) return la;
            const auto lb = load_xmm_value(n.data_inputs[1], Xmm::XMM1);
            if (!lb) return lb;
            asm_.ucomisd(Xmm::XMM0, Xmm::XMM1);
            materialize_bool_f64(loc, ordered_cc(n.aux));
            return support::ok();
        }
        const auto la = load_value(n.data_inputs[0], kScratch);
        if (!la) return la;
        const NodeId b = n.data_inputs[1];
        const Location& bloc = alloc_.location[b];
        switch (bloc.kind) {
        case Location::Kind::Gp:
            asm_.cmp_reg_reg(kScratch, loc_reg(bloc));
            break;
        case Location::Kind::Spill:
            asm_.cmp_reg_mem(
                kScratch,
                Mem{Reg::RBP, Reg::RSP, 0, slot_disp(bloc.slot)});
            break;
        default:
            asm_.cmp_reg_imm32(
                kScratch, static_cast<int32_t>(const_word(g_.node(b))));
            break;
        }
        materialize_bool(loc, cc_of(static_cast<CondCode>(n.aux)));
        return support::ok();
    }

    /// Tagged-boolean materialization from flags. Integer form (J1
    /// store_boolean): cc_true selects the true path.
    void materialize_bool(const Location& loc, uint8_t cc) {
        const size_t t = asm_.placeholder_jcc(cc);
        store_const_to_loc(loc, kFalseBits);
        const size_t done = asm_.placeholder_jmp();
        asm_.bind_placeholder(t);
        store_const_to_loc(loc, kTrueBits);
        asm_.bind_placeholder(done);
    }

    /// Float form (J1 t_cmp_f64 shape): the NEGATED predicate and the
    /// parity (unordered) path both produce false; everything else true.
    void materialize_bool_f64(const Location& loc, uint8_t cc) {
        const size_t to_false = asm_.placeholder_jcc(negated_cc(cc));
        const size_t parity = asm_.placeholder_jcc(CC_P);
        store_const_to_loc(loc, kTrueBits);
        const size_t done = asm_.placeholder_jmp();
        asm_.bind_placeholder(to_false);
        asm_.bind_placeholder(parity);
        store_const_to_loc(loc, kFalseBits);
        asm_.bind_placeholder(done);
    }

    static uint8_t negated_cc(uint8_t cc) {
        return static_cast<uint8_t>(cc ^ 1);  // x86 inverts bit 0
    }

    // ---- guards ----------------------------------------------------------------
    /// Loads the guarded value into the scratch for the check (heap guards
    /// untangle the tagged word: &-16 recovers the raw pointer).
    support::Result<void> emit_type_guard(NodeId id, const Node& n) {
        const NodeId value = n.data_inputs[0];
        const Location& loc = alloc_.location[id];
        const auto kind = static_cast<GuardKind>(n.aux);
        switch (kind) {
        case GuardKind::Smi: {
            const auto lv = load_value(value, kScratch);
            if (!lv) return lv;
            asm_.test_reg_imm8(kScratch, 1);
            const size_t fail = asm_.placeholder_jcc(CC_NE);
            if (auto r = guard_fail(id, fail); !r) return r;
            break;
        }
        case GuardKind::Heap: {
            const auto lv = load_value(value, kScratch);
            if (!lv) return lv;
            asm_.and_reg_imm32(kScratch, static_cast<int32_t>(kTagMask));
            asm_.cmp_reg_imm32(kScratch,
                               static_cast<int32_t>(kTagHeapBits));
            const size_t fail = asm_.placeholder_jcc(CC_NE);
            if (auto r = guard_fail(id, fail); !r) return r;
            break;
        }
        case GuardKind::NonNull: {
            // Heap-guarded upstream; here: NOT the null word.
            const auto lv = load_value(value, kScratch);
            if (!lv) return lv;
            asm_.cmp_reg_imm32(kScratch,
                               static_cast<int32_t>(kNullBits));
            const size_t fail = asm_.placeholder_jcc(CC_E);
            if (auto r = guard_fail(id, fail); !r) return r;
            break;
        }
        case GuardKind::IsNull: {
            const auto lv = load_value(value, kScratch);
            if (!lv) return lv;
            asm_.cmp_reg_imm32(kScratch,
                               static_cast<int32_t>(kNullBits));
            const size_t fail = asm_.placeholder_jcc(CC_NE);
            if (auto r = guard_fail(id, fail); !r) return r;
            break;
        }
        case GuardKind::IsNonNull: {
            const auto lv = load_value(value, kScratch);
            if (!lv) return lv;
            asm_.cmp_reg_imm32(kScratch,
                               static_cast<int32_t>(kNullBits));
            const size_t fail = asm_.placeholder_jcc(CC_E);
            if (auto r = guard_fail(id, fail); !r) return r;
            break;
        }
        case GuardKind::BoxedF64: {
            const auto lv = load_value(value, kScratch);
            if (!lv) return lv;
            // heap + klass == double_klass (imm64 compare in a saved RDX).
            asm_.and_reg_imm32(kScratch, static_cast<int32_t>(kTagMask));
            asm_.cmp_reg_imm32(kScratch,
                               static_cast<int32_t>(kTagHeapBits));
            const size_t tag_fail = asm_.placeholder_jcc(CC_NE);
            if (auto r = guard_fail(id, tag_fail); !r) return r;
            const auto lv2 = load_value(value, kScratch);
            if (!lv2) return lv2;
            asm_.and_reg_imm32(kScratch, -16);
            asm_.mov_reg_mem(kScratch, Mem{kScratch, Reg::RSP, 0, 0});
            asm_.push_reg(Reg::RDX);
            asm_.mov_reg_imm64(
                Reg::RDX, reinterpret_cast<uintptr_t>(job_.double_klass));
            asm_.cmp_reg_reg(kScratch, Reg::RDX);
            asm_.pop_reg(Reg::RDX);
            const size_t klass_fail = asm_.placeholder_jcc(CC_NE);
            if (auto r = guard_fail(id, klass_fail); !r) return r;
            break;
        }
        }
        // Proven: the guard's value IS its input (copy into the guard's
        // location when the allocator separated them).
        copy_guard_value(loc, value);
        return support::ok();
    }

    support::Result<void> emit_class_guard(NodeId id, const Node& n) {
        const NodeId value = n.data_inputs[0];
        const Location& loc = alloc_.location[id];
        const auto lv = load_value(value, kScratch);
        if (!lv) return lv;
        asm_.and_reg_imm32(kScratch, static_cast<int32_t>(kTagMask));
        asm_.cmp_reg_imm32(kScratch, static_cast<int32_t>(kTagHeapBits));
        const size_t tag_fail = asm_.placeholder_jcc(CC_NE);
        if (auto r = guard_fail(id, tag_fail); !r) return r;
        const auto lv2 = load_value(value, kScratch);
        if (!lv2) return lv2;
        asm_.and_reg_imm32(kScratch, -16);
        asm_.mov_reg_mem(kScratch, Mem{kScratch, Reg::RSP, 0, 0});
        // Expected klass: the profiled id resolved through the klass table.
        const void* klass = nullptr;
        const uint32_t klass_id = n.aux;
        if (job_.klass_addrs != nullptr &&
            klass_id < job_.klass_addrs->size()) {
            klass = (*job_.klass_addrs)[klass_id];
        }
        asm_.push_reg(Reg::RDX);
        asm_.mov_reg_imm64(Reg::RDX, reinterpret_cast<uintptr_t>(klass));
        asm_.cmp_reg_reg(kScratch, Reg::RDX);
        asm_.pop_reg(Reg::RDX);
        const size_t klass_fail = asm_.placeholder_jcc(CC_NE);
        if (auto r = guard_fail(id, klass_fail); !r) return r;
        copy_guard_value(loc, value);
        return support::ok();
    }

    void copy_guard_value(const Location& loc, NodeId value) {
        if (!loc.valid()) return;
        if (is_const(value)) {
            emit_move_const(loc, g_.node(value));
            return;
        }
        const Location& src = alloc_.location[value];
        if (!src.valid()) return;
        emit_move(loc, src);
    }

    uint32_t position_of(NodeId id) const {
        return id < position_.size() ? position_[id] : UINT32_MAX;
    }

    /// True when `consumer` is the LAST reader of `v` (the value's interval
    /// ends at the consumer's position) — the emitter may then mutate the
    /// value's own register in place.
    bool is_last_use(NodeId v, NodeId consumer) const {
        if (v >= g_.node_count()) {
            fprintf(stderr,
                    "[j2-bug] is_last_use bad v %u (consumer %u)\n", v,
                    consumer);
        }
        if (is_const(v)) return true;
        const Allocation::IntervalView* iv = interval_of(v);
        if (iv == nullptr) return true;
        return iv->end <= position_of(consumer);
    }

    /// Counts how many times `consumer` lists `v` as a data input — an
    /// operand may appear twice (Mul(v, v)); mutating it in place once
    /// would corrupt the second read.
    uint32_t uses_in(NodeId v, NodeId consumer) const {
        uint32_t n = 0;
        for (const NodeId in : g_.node(consumer).data_inputs) {
            if (in == v) ++n;
        }
        return n;
    }

    struct Staged {
        Reg reg;
        bool pop_rdx = false;  // balance after the consuming op
    };

    /// Stages `v` into a register SAFE TO MUTATE (applying sar 1 when
    /// `untag`). Preference order:
    ///   1. the value's own register when `consumer` is its last read
    ///      (mutating in place frees the allocator's copy discipline);
    ///   2. a copy in `into` (safe only when `into` is backend-reserved);
    ///   3. otherwise a copy through push/pop-saved RDX — no live value
    ///      ever occupies it across the push.
    /// The caller pops RDX AFTER the consuming instruction (Staged::pop_rdx).
    support::Result<Staged> stage_operand(NodeId v, NodeId consumer,
                                          Reg into, bool untag,
                                          bool into_reserved) {
        if (is_const(v)) {
            const Reg dst = into_reserved ? into : Reg::RDX;
            if (!into_reserved) asm_.push_reg(Reg::RDX);
            store_const_to_reg(dst, const_word(g_.node(v)));
            if (untag) asm_.sar_reg_imm8(dst, 1);
            return Staged{dst, !into_reserved};
        }
        const Location& loc = alloc_.location[v];
        if (loc.kind == Location::Kind::Spill) {
            const Reg dst = into_reserved ? into : Reg::RDX;
            if (!into_reserved) asm_.push_reg(Reg::RDX);
            asm_.mov_reg_mem(dst,
                             Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)});
            if (untag) asm_.sar_reg_imm8(dst, 1);
            return Staged{dst, !into_reserved};
        }
        if (loc.kind != Location::Kind::Gp) {
            return support::fail(support::ErrorCode::InternalError,
                                 "J2: no location for operand " +
                                     std::to_string(v));
        }
        const Reg src = loc_reg(loc);
        if (is_last_use(v, consumer) && uses_in(v, consumer) == 1) {
            if (untag) asm_.sar_reg_imm8(src, 1);
            return Staged{src, false};
        }
        if (src != into && into_reserved) {
            asm_.mov_reg_reg(into, src);
            if (untag) asm_.sar_reg_imm8(into, 1);
            return Staged{into, false};
        }
        asm_.push_reg(Reg::RDX);
        asm_.mov_reg_reg(Reg::RDX, src);
        if (untag) asm_.sar_reg_imm8(Reg::RDX, 1);
        return Staged{Reg::RDX, true};
    }

    support::Result<void> emit_bounds_guard(NodeId id, const Node& n) {
        // inputs: [index (smi-guarded), length (smi-guarded), FrameState].
        // Payload-domain unsigned compare: negative indices fail (huge
        // unsigned), index >= length fails — T0 re-executes on deopt and
        // raises its canonical bounds error (Rule 30).
        auto li = stage_operand(n.data_inputs[0], id, kScratch,
                                /*untag=*/true, /*into_reserved=*/true);
        if (!li) return std::unexpected(li.error());
        auto ll = stage_operand(n.data_inputs[1], id, Reg::RDX,
                                /*untag=*/true, /*into_reserved=*/false);
        if (!ll) return std::unexpected(ll.error());
        asm_.cmp_reg_reg(li->reg, ll->reg);
        if (ll->pop_rdx) asm_.pop_reg(Reg::RDX);
        if (li->pop_rdx) asm_.pop_reg(Reg::RDX);
        const size_t fail = asm_.placeholder_jcc(CC_AE);
        if (auto r = guard_fail(id, fail); !r) return r;
        return support::ok();
    }

    // ---- memory ------------------------------------------------------------------
    /// Untangles a tagged heap word into the raw base pointer in the
    /// scratch (the guard upstream proved the heap tag).
    support::Result<void> emit_load(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        const auto kind = static_cast<AccessKind>(n.aux);
        switch (kind) {
        case AccessKind::DoublePayload: {
            const auto lo = load_untangled(n.data_inputs[0]);
            if (!lo) return lo;
            asm_.movsd_xmm_mem(Xmm::XMM0,
                               Mem{kScratch, Reg::RSP, 0,
                                   kObjectHeaderBytes});
            store_xmm_result(loc, Xmm::XMM0);
            return support::ok();
        }
        case AccessKind::Field: {
            // Generic field read through the T0 helper (ADR-005 contract:
            // the helper writes `out`, the return code is the only failure
            // channel — a stored Smi 0 is data). The context-indirect call
            // is a safepoint for the allocator: homes are written and the
            // GC map is recorded here (Rule 78/86).
            if (auto r = emit_safepoint_state(id); !r) return r;
            const auto lo = load_value(n.data_inputs[0], Reg::RSI);
            if (!lo) return lo;
            asm_.mov_reg_reg(Reg::RDI, Reg::R15);
            asm_.mov_reg_imm32sx(Reg::RDX,
                                 static_cast<int32_t>(n.const_value));
            // out = the call window's out slot (offset 0 — nothing staged).
            asm_.lea_reg_mem(Reg::RCX,
                             Mem{Reg::RBP, Reg::RSP, 0, call_window_disp_});
            asm_.call_mem(Mem{Reg::R15, Reg::RSP, 0, kCtxGetFieldSlow});
            asm_.test_reg_imm32(kScratch, -1);
            const size_t err = asm_.placeholder_jcc(CC_E);
            error_fixups_.push_back({err, kErrGetField});
            asm_.mov_reg_mem(kScratch,
                             Mem{Reg::RBP, Reg::RSP, 0, call_window_disp_});
            store_result(loc, kScratch);
            return support::ok();
        }
        case AccessKind::RawOffset: {
            const auto lo = load_untangled(n.data_inputs[0]);
            if (!lo) return lo;
            const int32_t off = static_cast<int32_t>(n.const_value);
            asm_.mov_reg_mem(kScratch, Mem{kScratch, Reg::RSP, 0, off});
            store_result(loc, kScratch);
            return support::ok();
        }
        case AccessKind::ConstPool: {
            asm_.mov_reg_mem(kScratch,
                             Mem{Reg::R15, Reg::RSP, 0, kCtxConstants});
            const int32_t byte_off = static_cast<int32_t>(8 * n.const_value);
            asm_.mov_reg_mem(kScratch, Mem{kScratch, Reg::RSP, 0, byte_off});
            store_result(loc, kScratch);
            return support::ok();
        }
        case AccessKind::ConstPoolF64: {
            asm_.mov_reg_mem(kScratch,
                             Mem{Reg::R15, Reg::RSP, 0, kCtxConstants});
            const int32_t byte_off = static_cast<int32_t>(8 * n.const_value);
            asm_.movsd_xmm_mem(Xmm::XMM0,
                               Mem{kScratch, Reg::RSP, 0, byte_off});
            store_xmm_result(loc, Xmm::XMM0);
            return support::ok();
        }
        case AccessKind::ArrayElement: {
            // inputs: [array (heap-guarded), index (smi-guarded), fs].
            // Bounds: T0 re-executes the access on deopt and raises its
            // canonical error (Rule 30 — the check is a guard, not a trap).
            const uint32_t rec = record_of(id);
            if (g_j2_trace) {
                const Node& arr = g_.node(n.data_inputs[0]);
                const Location& al = alloc_.location[n.data_inputs[0]];
                fprintf(stderr,
                        "[j2-get] node=%u arr=%u(arrkind=%d loc=%d reg=%u "
                        "slot=%d) idx=%u\n",
                        id, n.data_inputs[0], (int)arr.kind, (int)al.kind,
                        al.reg, al.slot, n.data_inputs[1]);
            }
            const auto lo = load_untangled(n.data_inputs[0]);
            if (!lo) return lo;  // kScratch = untangled array base
            const auto li = load_value(n.data_inputs[1], Reg::RCX);
            if (!li) return li;
            asm_.sar_reg_imm8(Reg::RCX, 1);
            // 64-bit length compare reads length + zero pad (always 0), so
            // the word equals the u32 length; unsigned jae also traps
            // negative indices (T0-verbatim bounds, Rule 110).
            asm_.cmp_reg_mem(Reg::RCX, Mem{kScratch, Reg::RSP, 0,
                                           kArrayLengthOffset});
            const size_t fail = asm_.placeholder_jcc(CC_AE);
            if (rec == UINT32_MAX) {
                error_fixups_.push_back({fail, kErrArrayGet});
            } else {
                guard_sites_.push_back({id, fail, rec});
            }
            asm_.shl_reg_imm8(Reg::RCX, 3);  // idx * sizeof(TaggedValue)
            // scale 0 = *1 (the index is already byte-scaled; the SIB code
            // is NOT the shift amount).
            asm_.mov_reg_mem(kScratch,
                             Mem{kScratch, Reg::RCX, 0, kArrayHeaderBytes});
            store_result(loc, kScratch);
            return support::ok();
        }
        case AccessKind::ArrayLength: {
            const auto lo = load_untangled(n.data_inputs[0]);
            if (!lo) return lo;
            // Arrays carry klass == 0 in the header (the discriminator the
            // whole engine family tests — T0-verbatim).
            asm_.mov_reg_mem(kScratch, Mem{kScratch, Reg::RSP, 0, 0});
            asm_.test_reg_imm32(kScratch, -1);
            const size_t not_array = asm_.placeholder_jcc(CC_NE);
            error_fixups_.push_back({not_array, kErrArrayLen});
            const auto lb = load_untangled(n.data_inputs[0]);
            if (!lb) return lb;
            asm_.mov_reg32_mem(kScratch,
                               Mem{kScratch, Reg::RSP, 0,
                                   kArrayLengthOffset});
            asm_.shl_reg_imm8(kScratch, 1);  // u32 length -> Smi
            store_result(loc, kScratch);
            return support::ok();
        }
        }
        return support::fail(support::ErrorCode::Unimplemented,
                             "J2: unsupported access kind " +
                                 std::to_string(static_cast<uint32_t>(kind)));
    }

    support::Result<void> emit_store(NodeId id, const Node& n) {
        const auto kind = static_cast<AccessKind>(n.aux);
        switch (kind) {
        case AccessKind::Field: {
            // Generic store through the T0 helper (barrier included). The
            // context-indirect call is a safepoint for the allocator
            // (Rule 78/86 — homes + GC map, mirroring emit_load Field).
            if (auto r = emit_safepoint_state(id); !r) return r;
            const auto lo = load_value(n.data_inputs[0], Reg::RSI);
            if (!lo) return lo;
            const auto lv = load_value(n.data_inputs[1], Reg::RDX);
            if (!lv) return lv;
            asm_.mov_reg_reg(Reg::RDI, Reg::R15);
            asm_.mov_reg_imm32sx(Reg::RCX,
                                 static_cast<int32_t>(n.const_value));
            asm_.call_mem(Mem{Reg::R15, Reg::RSP, 0, kCtxSetFieldSlow});
            asm_.test_reg_imm32(kScratch, -1);
            const size_t err = asm_.placeholder_jcc(CC_E);
            error_fixups_.push_back({err, kErrSetField});
            return support::ok();
        }
        case AccessKind::RawOffset: {
            const auto lo = load_untangled(n.data_inputs[0]);
            if (!lo) return lo;
            const int32_t off = static_cast<int32_t>(n.const_value);
            const auto lv = load_value(n.data_inputs[1], Reg::RCX);
            if (!lv) return lv;
            asm_.mov_mem_reg(Mem{kScratch, Reg::RSP, 0, off}, Reg::RCX);
            if (auto r = emit_card_barrier(n.data_inputs[1]); !r) return r;
            return support::ok();
        }
        case AccessKind::ArrayElement: {
            // inputs: [array, index, value, fs]. Bounds -> deopt record.
            const uint32_t rec = record_of(id);
            const auto lo = load_untangled(n.data_inputs[0]);
            if (!lo) return lo;  // kScratch = untangled array base
            const auto li = load_value(n.data_inputs[1], Reg::RCX);
            if (!li) return li;
            asm_.sar_reg_imm8(Reg::RCX, 1);
            asm_.cmp_reg_mem(Reg::RCX, Mem{kScratch, Reg::RSP, 0,
                                           kArrayLengthOffset});
            const size_t fail = asm_.placeholder_jcc(CC_AE);
            if (rec == UINT32_MAX) {
                error_fixups_.push_back({fail, kErrArraySet});
            } else {
                guard_sites_.push_back({id, fail, rec});
            }
            const auto lv = load_value(n.data_inputs[2], Reg::RSI);
            if (!lv) return lv;
            asm_.shl_reg_imm8(Reg::RCX, 3);
            asm_.mov_mem_reg(Mem{kScratch, Reg::RCX, 0, kArrayHeaderBytes},
                             Reg::RSI);
            if (auto r = emit_card_barrier(n.data_inputs[2]); !r) return r;
            return support::ok();
        }
        default:
            break;
        }
        return support::fail(support::ErrorCode::Unimplemented,
                             "J2: unsupported store kind " +
                                 std::to_string(static_cast<uint32_t>(kind)));
    }

    /// ICGGC card-marking barrier for a reference store (the J1 shape:
    /// tag test, heap-base fold, >>kCardShift byte store). Skipped only
    /// when the type lattice PROVES the value is not a reference
    /// (Rule 80: elision requires proof).
    support::Result<void> emit_card_barrier(NodeId value) {
        const JType t = value < built_.types.size() ? built_.types[value]
                                                    : JType::Unknown;
        if (t == JType::Smi || t == JType::Bool || t == JType::Null) {
            return support::ok();  // proven non-ref by the type lattice
        }
        const auto lt = load_value(value, Reg::RCX);
        if (!lt) return lt;
        asm_.and_reg_imm32(Reg::RCX, static_cast<int32_t>(kTagMask));
        asm_.cmp_reg_imm32(Reg::RCX, static_cast<int32_t>(kTagHeapBits));
        const size_t done = asm_.placeholder_jcc(CC_NE);  // not a reference
        const auto lo = load_untangled(value);
        if (!lo) return lo;  // kScratch = untangled object base
        asm_.mov_reg_mem(Reg::RCX,
                         Mem{Reg::R15, Reg::RSP, 0, kCtxHeapBase});
        asm_.sub_reg_reg(kScratch, Reg::RCX);  // offset from heap origin
        asm_.shr_reg_imm8(kScratch, 9);        // >> kCardShift (ICGGC)
        asm_.mov_reg_mem(Reg::RCX,
                         Mem{Reg::R15, Reg::RSP, 0, kCtxCardBase});
        asm_.mov_mem_imm8(Mem{Reg::RCX, kScratch, 0, 0}, 1);  // DIRTY
        asm_.bind_placeholder(done);
        return support::ok();
    }

    support::Result<void> load_untangled(NodeId v) {
        const auto lv = load_value(v, kScratch);
        if (!lv) return lv;
        asm_.and_reg_imm32(kScratch, -16);
        return support::ok();
    }

    // ---- calls / allocation / safepoints ---------------------------------------
    support::Result<void> emit_call(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        if (auto r = emit_safepoint_state(id); !r) return r;
        const uint32_t shape = (n.aux >> 16) & 0x3;
        if (shape == static_cast<uint32_t>(CallShape::Generic)) {
            return emit_generic_call(id, n);
        }
        // Direct/Virtual -> invoke_method(token); Builtin ->
        // invoke_builtin(token). Both take a CONTIGUOUS TaggedValue
        // argument window (the J1 invoke ABI).
        const bool builtin = shape == static_cast<uint32_t>(CallShape::Builtin);
        const size_t argc = n.data_inputs.size() - 1;
        for (size_t i = 0; i < argc; ++i) {
            const NodeId arg = n.data_inputs[i];
            const int32_t win = call_window_disp_ +
                                static_cast<int32_t>(8 * i);
            if (is_const(arg)) {
                asm_.mov_reg_imm32sx(
                    kScratch,
                    static_cast<int32_t>(const_word(g_.node(arg))));
                asm_.mov_mem_reg(Mem{Reg::RBP, Reg::RSP, 0, win}, kScratch);
            } else {
                const Location& aloc = alloc_.location[arg];
                switch (aloc.kind) {
                case Location::Kind::Gp:
                    asm_.mov_mem_reg(Mem{Reg::RBP, Reg::RSP, 0, win},
                                     loc_reg(aloc));
                    break;
                case Location::Kind::Spill:
                    asm_.mov_reg_mem(kScratch,
                                     Mem{Reg::RBP, Reg::RSP, 0,
                                         slot_disp(aloc.slot)});
                    asm_.mov_mem_reg(Mem{Reg::RBP, Reg::RSP, 0, win},
                                     kScratch);
                    break;
                default:
                    return support::fail(
                        support::ErrorCode::InternalError,
                        "J2: call argument without a location");
                }
            }
        }
        // out = the window's out slot (offset 8*argc); args = the window
        // BASE (offset 0). invoke ABI (helper_invoke_*): rdi=ctx,
        // rsi=token, rdx=args, rcx=argc, r8=out — rdx and r8 are DIFFERENT
        // pointers; passing the out slot as the args base made the callee
        // read empty window tail slots and left the out slot stale.
        asm_.lea_reg_mem(Reg::R8,
                         Mem{Reg::RBP, Reg::RSP, 0,
                             call_window_disp_ +
                                 static_cast<int32_t>(8 * argc)});
        asm_.lea_reg_mem(Reg::RDX,
                         Mem{Reg::RBP, Reg::RSP, 0, call_window_disp_});
        asm_.mov_reg_imm32sx(Reg::RSI,
                             static_cast<int32_t>(n.aux & 0xFFFF));
        asm_.mov_reg_imm32sx(Reg::RCX, static_cast<int32_t>(argc));
        asm_.mov_reg_reg(Reg::RDI, Reg::R15);
        asm_.call_mem(Mem{Reg::R15, Reg::RSP, 0,
                          builtin ? kCtxInvokeBuiltin : kCtxInvokeMethod});
        asm_.test_reg_imm32(kScratch, -1);
        const size_t err = asm_.placeholder_jcc(CC_NE);
        // The invoke helpers return the error id AND record last_error:
        // branch straight to the epilogue with rax already correct.
        fixups_.push_back({FixupKind::Epilogue, err, 0, 0});
        asm_.mov_reg_mem(kScratch,
                         Mem{Reg::RBP, Reg::RSP, 0,
                             call_window_disp_ +
                                 static_cast<int32_t>(8 * argc)});
        store_result(loc, kScratch);
        return support::ok();
    }

    /// Shape::Generic — the shared canonical-binop helper (ctx, op, a, b).
    /// Returns the tagged result or kUndefinedRawBits (T0 raises the
    /// canonical error there; the emitter maps it to the matching id).
    support::Result<void> emit_generic_call(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        const uint32_t op_id = n.aux & 0xFFFF;
        const size_t argc = n.data_inputs.size() - 1;
        const auto la = load_value(n.data_inputs[0], Reg::RDX);
        if (!la) return la;
        if (argc > 1) {
            const auto lb = load_value(n.data_inputs[1], Reg::RCX);
            if (!lb) return lb;
        } else {
            asm_.xor_reg_reg(Reg::RCX, Reg::RCX);
        }
        asm_.mov_reg_reg(Reg::RDI, Reg::R15);
        asm_.mov_reg_imm32sx(Reg::RSI, static_cast<int32_t>(op_id));
        asm_.call_mem(Mem{Reg::R15, Reg::RSP, 0, kCtxGenericBinop});
        asm_.cmp_reg_imm32(kScratch, static_cast<int32_t>(kUndefinedRawBits));
        const size_t err = asm_.placeholder_jcc(CC_E);
        error_fixups_.push_back({err, helper_error_id(op_id)});
        store_result(loc, kScratch);
        return support::ok();
    }

    support::Result<void> emit_allocate(NodeId id, const Node& n) {
        const Location& loc = alloc_.location[id];
        if (auto r = emit_safepoint_state(id); !r) return r;
        if (n.aux == kAllocDouble) {
            // Boxed double: payload (xmm) + TLAB bump; the slow path
            // carries the payload bits through movq.
            const auto lp = load_xmm_value(n.data_inputs[0], Xmm::XMM0);
            if (!lp) return lp;
            asm_.mov_reg_mem(Reg::RDX,
                             Mem{Reg::R15, Reg::RSP, 0, kCtxTlabTop});
            asm_.mov_reg_reg(Reg::RCX, Reg::RDX);
            asm_.add_reg_imm32(Reg::RCX, kBoxedDoubleAllocBytes);
            asm_.cmp_reg_mem(Reg::RCX,
                             Mem{Reg::R15, Reg::RSP, 0, kCtxTlabEnd});
            const size_t slow = asm_.placeholder_jcc(CC_A);
            asm_.mov_mem_reg(Mem{Reg::R15, Reg::RSP, 0, kCtxTlabTop},
                             Reg::RCX);
            asm_.mov_reg_imm64(Reg::RSI,
                               reinterpret_cast<uintptr_t>(
                                   job_.double_klass));
            asm_.mov_mem_reg(Mem{Reg::RDX, Reg::RSP, 0, 0}, Reg::RSI);
            asm_.mov_mem_imm32(Mem{Reg::RDX, Reg::RSP, 0, 8},
                               kBoxedDoubleAllocBytes);
            asm_.mov_mem_imm32(Mem{Reg::RDX, Reg::RSP, 0, 12}, 0);
            asm_.movsd_mem_xmm(
                Mem{Reg::RDX, Reg::RSP, 0, kObjectHeaderBytes}, Xmm::XMM0);
            asm_.mov_reg_reg(kScratch, Reg::RDX);
            asm_.or_reg_imm8(kScratch, 1);
            const size_t join = asm_.placeholder_jmp();
            asm_.bind_placeholder(slow);
            emit_alloc_slow(kAllocDouble);
            asm_.test_reg_imm32(kScratch, -1);
            const size_t oom = asm_.placeholder_jcc(CC_E);
            error_fixups_.push_back({oom, kErrAllocOOM});
            asm_.or_reg_imm8(kScratch, 1);
            asm_.bind_placeholder(join);
            store_result(loc, kScratch);
            return support::ok();
        }
        if (n.aux == kAllocArray) {
            // New.Array: T0-verbatim length gates + inline bump + zeroing.
            const auto ll = load_value(n.data_inputs[0], kScratch);
            if (!ll) return ll;
            asm_.test_reg_imm8(kScratch, 1);
            const size_t bad = asm_.placeholder_jcc(CC_NE);
            error_fixups_.push_back({bad, kErrNewArrayLen});
            asm_.mov_reg_reg(Reg::RCX, kScratch);
            asm_.sar_reg_imm8(Reg::RCX, 1);
            asm_.cmp_reg_imm32(Reg::RCX, 0);
            const size_t neg = asm_.placeholder_jcc(CC_L);
            error_fixups_.push_back({neg, kErrNewArrayLen});
            asm_.cmp_reg_imm32(Reg::RCX, kJ2MaxArrayLength);
            const size_t too_big = asm_.placeholder_jcc(CC_G);
            error_fixups_.push_back({too_big, kErrNewArrayLen});
            asm_.mov_reg_reg(Reg::RDX, Reg::RCX);
            asm_.shl_reg_imm8(Reg::RDX, 3);
            asm_.add_reg_imm32(Reg::RDX, kArrayHeaderBytes);
            asm_.add_reg_imm32(Reg::RDX, 15);  // TLAB 16-byte round (both
            asm_.and_reg_imm32(Reg::RDX, -16); // engines; identity invariant)
            asm_.mov_reg_mem(Reg::RSI,
                             Mem{Reg::R15, Reg::RSP, 0, kCtxTlabTop});
            asm_.mov_reg_reg(Reg::RDI, Reg::RSI);
            asm_.add_reg_reg(Reg::RDI, Reg::RDX);
            asm_.cmp_reg_mem(Reg::RDI,
                             Mem{Reg::R15, Reg::RSP, 0, kCtxTlabEnd});
            const size_t slow = asm_.placeholder_jcc(CC_A);
            asm_.mov_mem_reg(Mem{Reg::R15, Reg::RSP, 0, kCtxTlabTop},
                             Reg::RDI);
            asm_.mov_mem_imm32sx(Mem{Reg::RSI, Reg::RSP, 0, 0}, 0);
            asm_.mov_mem_reg(Mem{Reg::RSI, Reg::RSP, 0, 8}, Reg::RDX);
            asm_.mov_mem_reg(Mem{Reg::RSI, Reg::RSP, 0, 16}, Reg::RCX);
            // Zero-fill elements to tagged null (T0 allocator parity).
            asm_.mov_reg_reg(Reg::R8, Reg::RSI);
            asm_.add_reg_imm32(Reg::R8, kArrayHeaderBytes);
            asm_.cmp_reg_imm32(Reg::RCX, 0);
            const size_t no_elems = asm_.placeholder_jcc(CC_E);
            const size_t zero_top = asm_.current_offset();
            asm_.mov_mem_imm32sx(Mem{Reg::R8, Reg::RSP, 0, 0},
                                 static_cast<int32_t>(kNullBits));
            asm_.add_reg_imm32(Reg::R8, 8);
            asm_.dec_reg(Reg::RCX);
            const size_t zero_again = asm_.placeholder_jcc(CC_NE);
            backward_fixups_.push_back({zero_again, zero_top});
            asm_.bind_placeholder(no_elems);
            asm_.mov_reg_reg(kScratch, Reg::RSI);
            asm_.or_reg_imm8(kScratch, 1);
            const size_t join = asm_.placeholder_jmp();
            asm_.bind_placeholder(slow);
            // RCX still holds the untagged length (nothing between the
            // TLAB-miss branch and here touched it) — no reload from a
            // possibly-clobbered value location.
            asm_.mov_reg_reg(Reg::RDI, Reg::R15);
            asm_.mov_reg_imm32sx(Reg::RSI,
                                 static_cast<int32_t>(kAllocArray));
            asm_.mov_reg_reg(Reg::RDX, Reg::RCX);
            asm_.xor_reg_reg(Reg::RCX, Reg::RCX);
            asm_.call_mem(Mem{Reg::R15, Reg::RSP, 0, kCtxAllocSlow});
            asm_.test_reg_imm32(kScratch, -1);
            const size_t oom = asm_.placeholder_jcc(CC_E);
            error_fixups_.push_back({oom, kErrAllocOOM});
            asm_.or_reg_imm8(kScratch, 1);
            asm_.bind_placeholder(join);
            store_result(loc, kScratch);
            return support::ok();
        }
        // kAllocObject: klass token rides aux.
        const uint32_t klass_token = n.aux;
        uint32_t field_count = 0;
        if (job_.klass_addrs != nullptr &&
            klass_token < job_.klass_addrs->size() &&
            (*job_.klass_addrs)[klass_token] != nullptr) {
            field_count =
                static_cast<Klass*>((*job_.klass_addrs)[klass_token])
                    ->field_count();
        }
        const int32_t alloc_bytes =
            ((kObjectHeaderBytes + static_cast<int32_t>(8 * field_count)) +
             15) &
            ~15;
        asm_.mov_reg_mem(Reg::RDX,
                         Mem{Reg::R15, Reg::RSP, 0, kCtxTlabTop});
        asm_.lea_reg_mem(Reg::RCX,
                         Mem{Reg::RDX, Reg::RSP, 0, alloc_bytes});
        asm_.cmp_reg_mem(Reg::RCX, Mem{Reg::R15, Reg::RSP, 0, kCtxTlabEnd});
        const size_t slow = asm_.placeholder_jcc(CC_A);
        asm_.mov_mem_reg(Mem{Reg::R15, Reg::RSP, 0, kCtxTlabTop}, Reg::RCX);
        const void* klass = nullptr;
        if (job_.klass_addrs != nullptr &&
            klass_token < job_.klass_addrs->size()) {
            klass = (*job_.klass_addrs)[klass_token];
        }
        asm_.mov_reg_imm64(Reg::RSI, reinterpret_cast<uintptr_t>(klass));
        asm_.mov_mem_reg(Mem{Reg::RDX, Reg::RSP, 0, 0}, Reg::RSI);
        asm_.mov_mem_imm32(Mem{Reg::RDX, Reg::RSP, 0, 8}, alloc_bytes);
        asm_.mov_mem_imm32(Mem{Reg::RDX, Reg::RSP, 0, 12}, 0);
        // Zero fields to tagged null (T0 allocator parity).
        if (field_count > 0) {
            asm_.mov_reg_reg(Reg::RSI, Reg::RDX);
            asm_.add_reg_imm32(Reg::RSI, kObjectHeaderBytes);
            asm_.mov_reg_imm32sx(Reg::RCX, static_cast<int32_t>(field_count));
            const size_t zero_top = asm_.current_offset();
            asm_.mov_mem_imm32sx(Mem{Reg::RSI, Reg::RSP, 0, 0},
                                 static_cast<int32_t>(kNullBits));
            asm_.add_reg_imm32(Reg::RSI, 8);
            asm_.dec_reg(Reg::RCX);
            const size_t zero_again = asm_.placeholder_jcc(CC_NE);
            backward_fixups_.push_back({zero_again, zero_top});
        }
        asm_.mov_reg_reg(kScratch, Reg::RDX);
        asm_.or_reg_imm8(kScratch, 1);
        const size_t join = asm_.placeholder_jmp();
        asm_.bind_placeholder(slow);
        asm_.mov_reg_reg(Reg::RDI, Reg::R15);
        asm_.mov_reg_imm32sx(Reg::RSI, static_cast<int32_t>(kAllocObject));
        asm_.mov_reg_imm32sx(Reg::RDX, static_cast<int32_t>(klass_token));
        asm_.mov_reg_imm32sx(Reg::RCX, static_cast<int32_t>(field_count));
        asm_.call_mem(Mem{Reg::R15, Reg::RSP, 0, kCtxAllocSlow});
        asm_.test_reg_imm32(kScratch, -1);
        const size_t oom = asm_.placeholder_jcc(CC_E);
        error_fixups_.push_back({oom, kErrAllocOOM});
        asm_.or_reg_imm8(kScratch, 1);
        asm_.bind_placeholder(join);
        store_result(loc, kScratch);
        return support::ok();
    }

    /// alloc_slow call (slow path of every inline allocation): rdi = ctx,
    /// rsi = kind, rdx/rcx = kind args. RSI's klass pointer (double box)
    /// is only meaningful for kAllocDouble and is overwritten here — the
    /// fast-path header stores happen BEFORE this call.
    void emit_alloc_slow(uint32_t kind) {
        asm_.mov_reg_reg(Reg::RDI, Reg::R15);
        asm_.mov_reg_imm32sx(Reg::RSI, static_cast<int32_t>(kind));
        if (kind == kAllocDouble) {
            asm_.movq_gpr_xmm(Reg::RDX, Xmm::XMM0);  // payload bits
            asm_.xor_reg_reg(Reg::RCX, Reg::RCX);
        }
        asm_.call_mem(Mem{Reg::R15, Reg::RSP, 0, kCtxAllocSlow});
    }

    support::Result<void> emit_safepoint_poll(NodeId id) {
        if (auto r = emit_safepoint_state(id); !r) return r;
        // J1 suspension contract: the poll word is a pointer to a u32; a
        // nonzero word exits with the deopt id (no capture — run_j2 reruns
        // the method in T0 from the entry).
        asm_.mov_reg_mem(Reg::RCX,
                         Mem{Reg::R15, Reg::RSP, 0, kCtxSafepointWord});
        asm_.mov_reg32_mem(kScratch, Mem{Reg::RCX, Reg::RSP, 0, 0});
        asm_.test_reg_imm32(kScratch, -1);
        const size_t armed = asm_.placeholder_jcc(CC_NE);
        error_fixups_.push_back({armed, kErrDeopt});
        return support::ok();
    }

    /// Home-slot stores + GC-map bits for one safepoint (Rule 78/86):
    /// every reference live across the point is mapped — spill-resident
    /// refs map through their own slot, register-resident ones through the
    /// home slot the allocator reserved.
    support::Result<void> emit_safepoint_state(NodeId id) {
        const uint32_t pos = position_of(id);
        if (pos == UINT32_MAX) return support::ok();
        gc_map_bits_.assign((alloc_.spill_slots + 31) / 32, 0);
        bool any = false;
        for (const Allocation::IntervalView& iv : alloc_.intervals) {
            if (!(iv.start < pos && pos <= iv.end)) continue;
            const Location& loc = alloc_.location[iv.vreg];
            const JType t = built_.types[iv.vreg];
            const bool ref = is_ref_type(t);
            if (loc.kind == Location::Kind::Gp) {
                const int32_t home = alloc_.home_slot[iv.vreg];
                if (home < 0) continue;
                asm_.mov_mem_reg(
                    Mem{Reg::RBP, Reg::RSP, 0, slot_disp(home)},
                    loc_reg(loc));
                if (ref) {
                    gc_map_bits_[static_cast<size_t>(home) / 32] |=
                        1u << (static_cast<uint32_t>(home) % 32);
                    any = true;
                }
            } else if (loc.kind == Location::Kind::Spill) {
                if (iv.xmm) continue;  // raw payload, not a reference
                if (!ref) continue;
                gc_map_bits_[static_cast<size_t>(loc.slot) / 32] |=
                    1u << (static_cast<uint32_t>(loc.slot) % 32);
                any = true;
            }
        }
        if (any) {
            safepoint_sites_.push_back(
                {static_cast<uint32_t>(buf_.size()), gc_map_bits_});
        }
        return support::ok();
    }

    // ---- deopt stubs -------------------------------------------------------------
    /// One stub per guard record: materializes EVERY frame's vregs into
    /// the deopt window (push through the reserved scratch — no live value
    /// sits in it), calls the j2_deopt hook (ctx, record, window), and
    /// exits with the deopt id. State-exact resume happens in the runtime
    /// (Rules 39/40/42).
    size_t emit_deopt_stub(uint32_t record_idx) {
        const size_t stub = buf_.size();
        const DeoptRecord& rec = (*records_)[record_idx];
        // The stub materializes from the FrameState NODES (their value
        // locations); frame_nodes_[record_idx] holds them in record order
        // (captured while the records were built).
        const std::vector<NodeId>& frame_nodes = frame_nodes_[record_idx];
        for (size_t f = 0; f < rec.frames.size(); ++f) {
            const Node& fs = g_.node(frame_nodes[f]);
            for (uint32_t i = 0; i < fs.data_inputs.size(); ++i) {
                const NodeId v = fs.data_inputs[i];
                const int32_t win = deopt_window_disp_ +
                                    static_cast<int32_t>(
                                        8 * (rec.frames[f].vreg_base + i));
                if (is_const(v)) {
                    store_const_to_reg(
                        kScratch, const_word(g_.node(v)));
                } else {
                    const Location& loc = alloc_.location[v];
                    switch (loc.kind) {
                    case Location::Kind::Gp:
                        asm_.mov_reg_reg(kScratch, loc_reg(loc));
                        break;
                    case Location::Kind::Spill:
                        asm_.mov_reg_mem(
                            kScratch,
                            Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)});
                        break;
                    case Location::Kind::Xmm:
                        asm_.movq_gpr_xmm(kScratch, xmm_of(loc.reg));
                        break;
                    default:
                        asm_.mov_reg_imm32sx(
                            kScratch,
                            static_cast<int32_t>(kUndefinedBits));
                        break;
                    }
                }
                asm_.mov_mem_reg(Mem{Reg::RBP, Reg::RSP, 0, win}, kScratch);
            }
        }
        asm_.mov_reg_reg(Reg::RDI, Reg::R15);  // ctx
        asm_.mov_reg_imm64(
            Reg::RSI, reinterpret_cast<uintptr_t>(&(*records_)[record_idx]));
        asm_.lea_reg_mem(Reg::RDX,
                         Mem{Reg::RBP, Reg::RSP, 0, deopt_window_disp_});
        asm_.call_mem(Mem{Reg::R15, Reg::RSP, 0, kCtxJ2Deopt});
        asm_.mov_reg_imm32sx(Reg::RAX, static_cast<int32_t>(kErrDeopt));
        jmp_fixup(FixupKind::Epilogue);
        return stub;
    }

    // ---- OSR stubs (docs/tier-j2.md section 5; J1OsrFn ABI) --------------
    support::Result<void> emit_osr_stubs() {
        if (built_.osr_block_offsets.empty()) return support::ok();
        osr_offset_ = static_cast<uint32_t>(buf_.size());
        for (const uint32_t pc : built_.osr_block_offsets) {
            osr_pcs_.push_back(pc);
            uint32_t target_block = UINT32_MAX;
            for (uint32_t b = 0; b < built_.blocks.size(); ++b) {
                if (built_.blocks[b].bytecode_begin == pc) {
                    target_block = b;
                    break;
                }
            }
            if (target_block == UINT32_MAX) {
                return support::fail(support::ErrorCode::InternalError,
                                     "J2: OSR pc without a block");
            }
            const std::vector<NodeId>& entry_values =
                built_.block_entry_values[target_block];
            // Frame (no arg copy): the OSR ABI fills the register file
            // from the T0 snapshot (rdi = ctx, rsi = vreg_state,
            // rcx = ret). Same save set / alignment as the entry prologue.
            asm_.push_reg(Reg::RBP);
            asm_.mov_reg_reg(Reg::RBP, Reg::RSP);
            asm_.push_reg(Reg::R15);
            asm_.push_reg(Reg::R14);
            asm_.push_reg(Reg::R13);
            asm_.push_reg(Reg::R12);
            asm_.push_reg(Reg::RBX);
            const size_t sub_site = asm_.current_offset() + 3;
            asm_.sub_reg_imm32(Reg::RSP, 0);
            fixups_.push_back({FixupKind::Frame, sub_site, 0, 0});
            asm_.mov_reg_reg(Reg::R15, Reg::RDI);
            asm_.mov_reg_reg(Reg::R14, Reg::RCX);
            for (uint32_t v = 0; v < built_.register_count; ++v) {
                if (v >= entry_values.size()) break;
                const NodeId ev = entry_values[v];
                if (ev == kNoNode || ev >= g_.node_count() ||
                    is_const(ev)) {
                    continue;
                }
                const Location& loc = alloc_.location[ev];
                if (!loc.valid()) continue;
                asm_.mov_reg_mem(kScratch,
                                 Mem{Reg::RSI, Reg::RSP, 0,
                                     static_cast<int32_t>(8 * v)});
                switch (loc.kind) {
                case Location::Kind::Gp:
                    asm_.mov_reg_reg(loc_reg(loc), kScratch);
                    break;
                case Location::Kind::Spill:
                    asm_.mov_mem_reg(
                        Mem{Reg::RBP, Reg::RSP, 0, slot_disp(loc.slot)},
                        kScratch);
                    break;
                default:
                    break;
                }
            }
            jmp_fixup(FixupKind::Block, target_block);
        }
        return support::ok();
    }

    // ---- error blocks + metadata serialization ---------------------------------
    /// Shared out-of-line error exits, one per error id used: rax = id,
    /// ctx->last_error = id, exit. Error-fixup jccs patch here.
    size_t error_block_for(uint32_t err_id) {
        const auto it = error_blocks_.find(err_id);
        if (it != error_blocks_.end()) return it->second;
        const size_t at = buf_.size();
        asm_.mov_reg_imm32sx(Reg::RAX, static_cast<int32_t>(err_id));
        asm_.mov_mem_imm32(Mem{Reg::R15, Reg::RSP, 0, kCtxLastError},
                           static_cast<int32_t>(err_id));
        jmp_fixup(FixupKind::Epilogue);
        error_blocks_.emplace(err_id, at);
        return at;
    }

    std::vector<uint8_t> serialize_gc_maps() const {
        std::vector<uint8_t> out;
        append_u32(out, static_cast<uint32_t>(safepoint_sites_.size()));
        for (const auto& [site, bits] : safepoint_sites_) {
            append_u32(out, site);
            append_u32(out, static_cast<uint32_t>(bits.size()));
            for (const uint32_t w : bits) append_u32(out, w);
        }
        return out;
    }

    std::vector<uint8_t> serialize_deopt_records() const {
        std::vector<uint8_t> out;
        append_u32(out, static_cast<uint32_t>(records_->size()));
        for (const DeoptRecord& rec : *records_) {
            const size_t len_at = out.size();
            append_u32(out, 0);  // length placeholder
            append_u32(out, static_cast<uint32_t>(rec.frames.size()));
            for (const DeoptFrame& fr : rec.frames) {
                append_u32(out, fr.method_id);
                append_u32(out, fr.resume_pc);
                append_u32(out, fr.inject_dst);
                append_u32(out, fr.vreg_base);
                append_u32(out, fr.vreg_count);
            }
            const uint32_t len =
                static_cast<uint32_t>(out.size() - len_at);
            std::memcpy(out.data() + len_at, &len, 4);
        }
        return out;
    }

    static void append_u32(std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i < 4; ++i) {
            v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
        }
    }

    // ---- state -------------------------------------------------------------------
    const ir::Graph& g_;
    const BuiltGraph& built_;
    const std::vector<uint32_t>& layout_;
    const Allocation& alloc_;
    const J2Job& job_;
    const EmissionPlan& plan_;  // declared here to match init-list order
    std::shared_ptr<std::vector<DeoptRecord>> records_;
    std::vector<std::vector<NodeId>> frame_nodes_;  // per record

    CodeBuffer buf_;
    Assembler asm_;
    std::vector<Fixup> fixups_;
    struct BackwardFixup {
        size_t at;
        size_t target;
    };
    std::vector<BackwardFixup> backward_fixups_;
    struct ErrorFixup {
        size_t at;
        uint32_t err_id;
    };
    std::vector<ErrorFixup> error_fixups_;
    std::unordered_map<uint32_t, size_t> error_blocks_;
    std::unordered_map<uint32_t, size_t> block_offsets_;
    std::unordered_map<uint32_t, size_t> layout_pos_;
    std::unordered_map<uint32_t, uint32_t> record_index_of_;
    std::vector<uint32_t> position_;
    size_t epilogue_offset_ = 0;
    uint32_t osr_offset_ = 0xFFFFFFFFu;
    std::vector<uint32_t> osr_pcs_;
    int32_t locals_bytes_ = 0;
    int32_t deopt_window_disp_ = 0;
    int32_t call_window_disp_ = 0;
    uint32_t deopt_window_vregs_ = 0;
    uint32_t call_window_args_ = 0;
    bool rcx_reserved_ = false;
    std::vector<GuardSite> guard_sites_;
    struct SafepointSite {
        uint32_t site;
        std::vector<uint32_t> bits;
    };
    std::vector<SafepointSite> safepoint_sites_;
    std::vector<uint32_t> gc_map_bits_;
};


// @hot — the deopt entry trampoline (CEM-26 scope: hot). Copies the deopt
// window into the capture so the runtime can chain Interpreter::resume
// innermost-first. PERF_CONTRACT:
// BUDGET: window copy (vregs * 8 bytes) + one small-vector assign
// READS: window words; WRITES: the capture
// BRANCHES: 0 (straight-line copy)
// CACHE: window and capture are frame-sized (< 1 KiB for the depth cap)
// The window assign allocates through the controlled deopt-materialization
// path (Rule 67's sanctioned exception: deopt may allocate).
struct DeoptCapture {
    const DeoptRecord* record = nullptr;
    std::vector<TaggedValue> window;
    bool valid = false;
};


DeoptCapture& deopt_capture() {
    // Single-mutator M2 contract (ADR-002): one mutator thread owns the
    // engine; the M2 threading milestone replaces this with a per-thread
    // slot (docs/infrastructure/04-threading-suspension.md).
    static DeoptCapture capture;
    return capture;
}

void j2_deopt_hook(j1::J1Context* ctx, const DeoptRecord* record,
                   const TaggedValue* window) noexcept {
    if (g_j2_trace) {
        fprintf(stderr, "[j2-deopt] record=%p frames=%zu\n", (void*)record,
                record->frames.size());
        for (const DeoptFrame& fr : record->frames) {
            fprintf(stderr, "  frame method=%u pc=%u base=%u n=%u:",
                    fr.method_id, fr.resume_pc, fr.vreg_base, fr.vreg_count);
            for (uint32_t i = 0; i < fr.vreg_count; ++i) {
                fprintf(stderr, " %llx",
                        (unsigned long long)window[fr.vreg_base + i].raw());
            }
            fprintf(stderr, "\n");
        }
    }
    DeoptCapture& c = deopt_capture();
    c.record = record;
    uint32_t count = 0;
    for (const DeoptFrame& fr : record->frames) count += fr.vreg_count;
    c.window.assign(window, window + count);
    c.valid = true;
    ctx->last_error = kErrDeopt;
}

// ---- orchestration ----------------------------------------------------------

}  // namespace

void set_j2_trace(bool enabled) noexcept { g_j2_trace = enabled; }

support::Result<J2Code> compile_j2(const J2Job& job) {
    if (job.module == nullptr ||
        job.method_id >= job.module->method_table.size()) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "J2: unknown method id " +
                                 std::to_string(job.method_id));
    }
    const ugb::UGBMethod& method =
        job.module->method_table[job.method_id];

    j2::GraphBuilderParams bp;
    bp.module = job.module;
    bp.method = &method;
    bp.profiles = job.profiles;
    bp.ics = job.ics;
    bp.klass_addrs = job.klass_addrs;
    bp.node_cap = job.node_cap;
    j2::BuildResult br = build_graph(bp);
    if (!br.ok || br.out == nullptr) {
        const support::ErrorCode code =
            br.error == BuildError::UnsupportedOpcode ||
                    br.error == BuildError::BudgetExceeded
                ? support::ErrorCode::Unimplemented
                : support::ErrorCode::DecodeError;
        return support::fail(code, std::string("J2: build refused: ") +
                                       build_error_message(br.error) +
                                       " at pc " +
                                       std::to_string(br.error_pc));
    }
    BuiltGraph& built = *br.out;

    j2::PipelineBudget budget;
    budget.node_cap = job.node_cap;
    budget.pass_control =
        static_cast<uint64_t>(PassAll) & ~job.pass_kill_switches;
    const j2::PipelineStats stats = run_pipeline(*built.graph, built, budget);

    // Backend placement: LCA-of-uses, then the profile-driven layout.
    built.block_of = dominance_placement(*built.graph, built);
    std::vector<uint32_t> layout = block_layout_order(built);

    // The emitter's fixed GP transients are reserved as a set — the ones
    // clobbered MID-SEQUENCE (no adjacent safepoint): RAX (universal
    // scratch), RCX (index/argument staging, array addressing, setcc),
    // RDX (generic-binop operand staging) and RSI (store-value scratch).
    // RDI stays allocatable: every write to it is immediately call-adjacent
    // (helper ABI ctx setup), and any value whose live range crosses a call
    // is already forced out of caller-saved registers by the
    // crosses-safepoint rule. Without reservations the pool starves — five
    // reserved registers plus six loop phis spilled every arithmetic
    // temporary — but each reserved register here corresponds to a real
    // emitter clobber an operand staging sequence would hit.
    constexpr uint32_t kReservedGp =
        (1u << static_cast<uint32_t>(PhysReg::RAX)) |
        (1u << static_cast<uint32_t>(PhysReg::RCX)) |
        (1u << static_cast<uint32_t>(PhysReg::RDX)) |
        (1u << static_cast<uint32_t>(PhysReg::RSI));

    // FrameState-only values: Rule 42's complete deopt snapshot references
    // EVERY vreg, so loop phis and other state-only nodes stay alive even
    // when no compiled code reads them. Their homes are touched only by the
    // cold deopt stub — forcing them to spill keeps the registers for the
    // arithmetic the hot loop actually runs.
    std::vector<NodeId> fs_only;
    {
        std::unordered_map<NodeId, uint32_t> non_fs_users;
        for (const Node& n : built.graph->nodes()) {
            if (n.dead || n.kind == NodeKind::FrameState) continue;
            for (const NodeId in : n.data_inputs) {
                ++non_fs_users[in];
            }
        }
        for (uint32_t id = 0; id < built.graph->node_count(); ++id) {
            const Node& n = built.graph->node(id);
            if (n.dead || n.kind == NodeKind::FrameState) continue;
            if (non_fs_users.count(id) != 0) continue;
            const NodeKind k = n.kind;
            if (k == NodeKind::Const || k == NodeKind::Start ||
                k == NodeKind::Region || k == NodeKind::Loop ||
                k == NodeKind::End || k == NodeKind::If ||
                k == NodeKind::Return || k == NodeKind::Parameter) {
                continue;  // no allocated home (constants rematerialize)
            }
            fs_only.push_back(id);
        }
    }
    // The emission plan: per-block definition-before-use order + tagged-arith
    // fusion decisions. Computed BEFORE register allocation so the interval
    // model sees the true program order (a value read after a call gets an
    // interval that crosses the call's safepoint -> callee-saved or spill).
    const EmissionPlan plan = plan_emission(*built.graph, built);
    if (plan.cyclic) {
        return support::fail(support::ErrorCode::InternalError,
                             "J2: emission schedule cycle (broken SSA graph)");
    }
    Allocation alloc = allocate_registers(*built.graph, built, layout,
                                          kReservedGp, &fs_only,
                                          /*reserve_xmm_temps=*/true, &plan);
    if (g_j2_trace) {
        for (uint32_t nid = 0; nid < built.graph->node_count(); ++nid) {
            const Node& nd = built.graph->node(nid);
            if (nd.dead) continue;
            if (nd.kind == ir::NodeKind::FrameState) {
                fprintf(stderr, "[j2-fs] fs=%u block_of=%u inputs=[", nid,
                        built.block_of[nid]);
                for (ir::NodeId in : nd.data_inputs) {
                    fprintf(stderr, " %u", in);
                }
                fprintf(stderr, " ]\n");
            }
            const Location& lc = alloc.location[nid];
            const char* k = lc.kind == Location::Kind::Gp ? "GP"
                            : lc.kind == Location::Kind::Spill
                                ? "SP"
                                : lc.kind == Location::Kind::Xmm ? "XM"
                                                                 : "--";
            const Allocation::IntervalView* iv = nullptr;
            for (const auto& cand : alloc.intervals) {
                if (cand.vreg == nid) { iv = &cand; break; }
            }
            fprintf(stderr,
                    "[j2-alloc] %u %d %s reg=%u slot=%d home=%d "
                    "iv=[%u,%u]\n",
                    nid, (int)nd.kind, k, lc.reg, lc.slot,
                    alloc.home_slot[nid],
                    iv ? iv->start : UINT32_MAX, iv ? iv->end : UINT32_MAX);
        }
    }

    auto records = std::make_shared<std::vector<DeoptRecord>>();
    Emitter em(*built.graph, built, layout, alloc, job, records, plan);
    support::Result<J2Code> code = em.emit();
    if (!code) return std::unexpected(code.error());
    // Graceful degradation (docs/tier-j2.md section 1): a budget stop keeps
    // the code valid — the graph just never finished optimizing.
    code->budget_exceeded = stats.budget_stop != 0;
    code->stats = stats;
    return code;
}

support::Result<J2Executable> publish_j2(const J2Code& code,
                                         infra::CodeRange* range) {
    if (code.code.empty()) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "J2: empty code buffer");
    }
    auto mem = infra::WritableCodeMemory::allocate(code.code.size(), range);
    if (!mem) return std::unexpected(mem.error());
    (*mem).write(code.code, 0);
    auto pub = (*mem).publish();
    if (!pub) return std::unexpected(pub.error());
    J2Executable ex;
    ex.memory = std::move(*mem);
    ex.entry = reinterpret_cast<J1EntryFn>(ex.memory.data() +
                                           code.entry_offset);
    if (code.osr_entry_offset != 0xFFFFFFFFu &&
        code.osr_entry_offset < code.code.size()) {
        ex.osr_entry = reinterpret_cast<J1OsrFn>(ex.memory.data() +
                                                 code.osr_entry_offset);
    }
    ex.method_id = code.method_id;
    ex.records = code.records;
    return ex;
}

support::Result<TaggedValue> run_j2(J2Executable& ex, j1::J1Bindings& bindings,
                                    std::span<const TaggedValue> args,
                                    vm::Interpreter& interp) {
    TaggedValue ret;
    auto* heap = static_cast<gc::Heap*>(bindings.context.heap);
    heap->sync_tlab_top(static_cast<uint8_t*>(bindings.context.tlab_top));

    // Install the J2 deopt hook (ctx+0x48), run, restore.
    void* prev_hook = nullptr;
    std::memcpy(&prev_hook,
                reinterpret_cast<const uint8_t*>(&bindings.context) +
                    kCtxJ2Deopt,
                sizeof(prev_hook));
    void* hook = reinterpret_cast<void*>(&j2_deopt_hook);
    std::memcpy(reinterpret_cast<uint8_t*>(&bindings.context) + kCtxJ2Deopt,
                &hook, sizeof(hook));
    deopt_capture().valid = false;
    const int64_t rc = ex.entry(&bindings.context, args.data(),
                                static_cast<uint32_t>(args.size()), &ret);
    std::memcpy(reinterpret_cast<uint8_t*>(&bindings.context) + kCtxJ2Deopt,
                &prev_hook, sizeof(prev_hook));
    bindings.context.tlab_top = heap->tlab_top();
    bindings.context.tlab_end = heap->tlab_end();
    if (g_j2_trace) {
        fprintf(stderr, "[j2-run] rc=%lld last_error=%u osr=%d\n",
                (long long)rc, bindings.context.last_error,
                ex.osr_entry != nullptr);
    }

    if (rc == static_cast<int64_t>(j1::J1ErrorId::kErrDeopt)) {
        DeoptCapture& c = deopt_capture();
        if (!c.valid || c.record == nullptr) {
            // No capture: the safepoint poll fired (M1 suspension
            // contract) — rerun the whole method in the attached T0.
            auto* module = static_cast<ugb::UGBModule*>(
                const_cast<void*>(bindings.context.module));
            if (module != nullptr &&
                ex.method_id < module->method_table.size()) {
                heap->sync_tlab_top(
                    static_cast<uint8_t*>(bindings.context.tlab_top));
                auto run = interp.run(
                    *module, module->method_table[ex.method_id].name, args);
                bindings.context.tlab_top = heap->tlab_top();
                bindings.context.tlab_end = heap->tlab_end();
                if (run) return run->value;
                return std::unexpected(run.error());
            }
            return support::fail(support::ErrorCode::RuntimeError,
                                 "J2: deopt without a capture");
        }
        // State-exact continuation (Rule 39): resume innermost-first,
        // threading the completed frame's value into the caller's inject
        // register. One code path with fresh execution (Rule 39 — the
        // resumed dispatch loop is Interpreter::resume).
        TaggedValue value = TaggedValue::undefined();
        const DeoptRecord& rec = *c.record;
        for (size_t f = 0; f < rec.frames.size(); ++f) {
            const DeoptFrame& fr = rec.frames[f];
            const TaggedValue* vregs = c.window.data() + fr.vreg_base;
            auto run = interp.resume(
                *static_cast<ugb::UGBModule*>(const_cast<void*>(
                    bindings.context.module)),
                fr.method_id,
                std::span<const TaggedValue>(vregs, fr.vreg_count),
                fr.resume_pc, fr.inject_dst, value);
            if (!run) return std::unexpected(run.error());
            value = *run;
        }
        return value;
    }

    if (rc != 0) {
        return support::fail(support::ErrorCode::RuntimeError,
                             std::string("J2 runtime error: ") +
                                 j1::diagnose_j1_error(
                                     bindings.context.last_error));
    }
    return ret;
}

}  // namespace vortex::j2
