// J1 x86-64 stencil corpus (docs/tier-j1.md sections 2-3, docs/roadmap.md M1).
//
// Every template is ASSEMBLED ONCE at corpus-build time with the project's
// own x86-64 assembler (src/codegen/x64/assembler.cpp) and instantiated by
// memcpy + patch — re-encoding per method would defeat the J1 latency budget
// (docs/tier-j1.md section 10).
//
// Template conventions (ABI: include/vortex/j1/context.hpp):
//   r15 = J1Context*, r14 = result TaggedValue* (callee-saved, set up by the
//   prologue BaselineJit emits). Vregs live at [rbp + vreg_disp(rc, i)];
//   templates carry the kTemplateVregDisp placeholder and one PatchSite per
//   displacement, so instantiation rewrites them per method. The pad in
//   frame_size() keeps every real displacement outside the disp8 window, so
//   all patch sites share one uniform disp32 encoding.
//
// Every template ENDS with a 5-byte `jmp rel32` whose BranchTarget operand
// selects the continuation:
//   0 = the instruction's branch target, 1 = the method error epilogue,
//   2 = the method ok epilogue, 3 = the next instruction's native offset.
// The uniform tail makes superstencil fusion mechanical:
// promote_superstencils drops the first stencil's continuation tail and
// splices the pair (templates communicate only through frame memory, so
// concatenation is semantically safe — docs/tier-j1.md section 3).
//
// Error exits store the J1ErrorId into ctx->last_error and jump to the error
// epilogue; the driver maps ids to T0-worded diagnostics. Template semantics
// mirror T0's handlers exactly (parity DoD, docs/roadmap.md M1): smi-only
// canonical arithmetic with distinct overflow/type errors, boxed-double
// float ops with TLAB allocation, saturating F64->I64, helper-based
// division. Opcodes outside the M1 core families (bitops, closures,
// exceptions, atomics beyond the poll) have no stencil: BaselineJit::compile
// fails with kErrNoStencil and the tier handoff keeps the method in T0 —
// safe rejection, never misexecution (Rule 3).
//
// @cold — the whole file runs once per process (corpus build). The cost
// contracts that matter live in the TEMPLATES; the dominant guest-loop
// paths (guards, barriers, allocation) carry their budget notes inline.
#include "vortex/j1/stencil_corpus.hpp"

#include <cstring>
#include <utility>

#include "vortex/codegen/x64/assembler.hpp"
#include "vortex/j1/context.hpp"
#include "vortex/runtime/object_model.hpp"
#include "vortex/ugb/opcode.hpp"

namespace vortex::j1 {

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
using codegen::x64::CC_O;
using codegen::x64::CC_P;
using codegen::x64::CC_S;
using ::vortex::codegen::CodeBuffer;
using codegen::x64::Mem;
using codegen::x64::Reg;
using codegen::x64::Xmm;

namespace {

// ---- ABI registers (j1/context.hpp) ------------------------------------------

constexpr Reg kCtx = Reg::R15;
constexpr Reg kRetPtr = Reg::R14;
constexpr Reg kA = Reg::RAX;   // primary value scratch
constexpr Reg kB = Reg::RCX;   // second operand scratch
constexpr Reg kC = Reg::RDX;   // third scratch (freely clobbered)
constexpr Reg kD = Reg::RSI;   // fourth scratch (survives into helper args)
constexpr Reg kE = Reg::R8;    // fifth scratch (value carrier for SET_FIELD)
constexpr Reg kF = Reg::R9;    // sixth scratch (allocation bump arithmetic)

// J1Context field offsets (static_assert-pinned in j1/context.hpp).
constexpr int32_t kCtxTlabTop = 0x08;
constexpr int32_t kCtxTlabEnd = 0x10;
constexpr int32_t kCtxCardBase = 0x18;
constexpr int32_t kCtxHeapBase = 0x20;
constexpr int32_t kCtxConstants = 0x28;
constexpr int32_t kCtxAllocSlow = 0x80;
constexpr int32_t kCtxGetFieldSlow = 0xA8;
constexpr int32_t kCtxSetFieldSlow = 0xB0;
constexpr int32_t kCtxInvokeMethod = 0x98;
constexpr int32_t kCtxInvokeBuiltin = 0xA0;
constexpr int32_t kCtxGenericBinop = 0xC0;
constexpr int32_t kCtxLastError = 0xD8;

// Tagged-value encoding immediates (support/tagged_value.hpp).
constexpr int32_t kTagMask = 0xF;
constexpr int32_t kTagHeapBits = 0b0001;
constexpr int32_t kBitsNull = 0x3;
constexpr int32_t kBitsUndefined = 0x7;
constexpr int32_t kBitsFalse = 0xB;
constexpr int32_t kBitsTrue = 0xF;

// Boxed-double / object layout (runtime/object_model.hpp). Allocation
// sizes are ROUNDED to the 16-byte TLAB alignment so consecutive inline
// bumps keep every object base aligned (TaggedValue::heap_pointer masks
// the low nibble).
constexpr int32_t kObjectHeaderBytes = static_cast<int32_t>(sizeof(ObjectHeader));
constexpr int32_t kBoxedDoubleBytes = kObjectHeaderBytes + 8;
constexpr int32_t kBoxedDoubleAllocBytes = (kBoxedDoubleBytes + 15) & ~15;
constexpr int32_t kArrayHeaderBytes =
    kObjectHeaderBytes + static_cast<int32_t>(ArrayObject::kLengthPadBytes);

// alloc_slow kinds (j1/context.hpp J1AllocSlowFn contract).
constexpr uint32_t kAllocObject = 0;
constexpr uint32_t kAllocArray = 1;
constexpr uint32_t kAllocDouble = 2;

// J1ErrorId imports (j1/context.hpp).
using E = J1ErrorId;

// BranchTarget operand selectors (corpus-wide convention).
constexpr uint8_t BT_Instr = 0;   // the instruction's branch target
constexpr uint8_t BT_Error = 1;   // method error epilogue
constexpr uint8_t BT_Ok = 2;      // method ok epilogue
constexpr uint8_t BT_Next = 3;    // next instruction

// VirtualRegister operand selectors.
constexpr uint8_t VR_Dst = 0;
constexpr uint8_t VR_Src0 = 1;
constexpr uint8_t VR_Src1 = 2;
constexpr uint8_t VR_Src2 = 3;

// ConstantIndex operands (0-7 documented in stencil_corpus.hpp; 8+ here).
constexpr uint8_t PA_ConstSmi = 0;
constexpr uint8_t PA_ConstPoolOff = 1;
constexpr uint8_t PA_KlassAddr = 3;
constexpr uint8_t PA_FieldByteOff = 4;
constexpr uint8_t PA_CallToken = 8;     // imm32 = callee method token
constexpr uint8_t PA_OpId = 9;          // imm32 = ugb::Op id for the helper
constexpr uint8_t PA_KlassToken = 10;   // imm32 = class token (alloc_slow)
constexpr uint8_t PA_MaxArrayLen = 11;  // imm32 = T0 max_array_length
constexpr uint8_t PA_FieldToken = 12;   // imm32 = field token (slow helpers)
constexpr uint8_t PA_CallArgc = 13;     // imm32 = argc literal (src1 field)
constexpr uint8_t PA_FieldCount = 5;    // imm32 = klass field count

// ---- template builder ---------------------------------------------------------

struct TemplateBuilder {
    CodeBuffer buf;
    Assembler asm_;
    Stencil out;

    TemplateBuilder(uint16_t opcode, bool speculative) : asm_(buf) {
        out.opcode = opcode;
        out.speculative = speculative;
    }

    void mark(PatchKind kind, uint8_t operand, size_t off) {
        PatchSite ps;
        ps.kind = kind;
        ps.offset = static_cast<uint32_t>(off);
        ps.operand = operand;
        out.patch_sites.push_back(ps);
    }

    size_t here() const noexcept { return buf.size(); }

    void load_vreg(Reg r, uint8_t sel) {
        const size_t off = here() + 3;  // REX + opcode + modrm, then disp32
        asm_.mov_reg_mem(r, Mem{Reg::RBP, Reg::RSP, 0, kTemplateVregDisp});
        mark(PatchKind::VirtualRegister, sel, off);
    }

    void store_vreg(Reg r, uint8_t sel) {
        const size_t off = here() + 3;
        asm_.mov_mem_reg(Mem{Reg::RBP, Reg::RSP, 0, kTemplateVregDisp}, r);
        mark(PatchKind::VirtualRegister, sel, off);
    }

    void lea_vreg(Reg r, uint8_t sel) {
        const size_t off = here() + 3;  // REX + 8D + modrm
        asm_.lea_reg_mem(r, Mem{Reg::RBP, Reg::RSP, 0, kTemplateVregDisp});
        mark(PatchKind::VirtualRegister, sel, off);
    }

    void imm64_site(Reg r, PatchKind kind, uint8_t operand) {
        const size_t off = here() + 2;  // REX + B8+rd
        asm_.mov_reg_imm64(r, 0);
        mark(kind, operand, off);
    }

    void imm32sx_site(Reg r, PatchKind kind, uint8_t operand) {
        const size_t off = here() + 3;  // REX + C7 + modrm
        asm_.mov_reg_imm32sx(r, 0);
        mark(kind, operand, off);
    }

    void ctx_store_imm32(int32_t ctx_off, int32_t value) {
        asm_.mov_mem_imm32(Mem{kCtx, Reg::RSP, 0, ctx_off}, value);
    }

    /// Binds a FORWARD placeholder to the current position (assembler-native).
    void bind(size_t ph) { asm_.bind_placeholder(ph); }

    /// Binds a placeholder to an arbitrary buffer position (either
    /// direction). The rel32 arithmetic is position-invariant under a
    /// uniform shift, so this equals the in-place assembler bind.
    void bind_at(size_t ph, size_t target) {
        const int32_t rel = static_cast<int32_t>(target - (ph + 4));
        std::memcpy(buf.code().data() + ph, &rel, 4);
    }

    /// Backward bind (reads better at loop sites).
    void bind_backward(size_t ph, size_t target) { bind_at(ph, target); }

    /// Error path: store the id, jump to the error epilogue.
    void error_exit(uint32_t err_id) {
        ctx_store_imm32(kCtxLastError, static_cast<int32_t>(err_id));
        tail(BT_Error);
    }

    /// The uniform continuation jump (see file header).
    void tail(uint8_t operand) {
        const size_t off = here() + 1;  // E9 + rel32
        asm_.jmp_rel32(0);
        mark(PatchKind::BranchTarget, operand, off);
    }

    Stencil finish() {
        out.bytes = std::move(buf.code());
        return std::move(out);
    }
};

// ---- shared sequences ----------------------------------------------------------

/// Both-operands-smi guard (kA/kB hold the tagged bits). Returns the
/// placeholder jcc for the caller to bind at the fallback/failure label.
size_t smi_guard(TemplateBuilder& b) {
    b.asm_.mov_reg_reg(kC, kA);
    b.asm_.or_reg_reg(kC, kB);
    b.asm_.test_reg_imm8(kC, 1);
    return b.asm_.placeholder_jcc(CC_NE);
}

/// Generic-binop helper call: generic_binop(ctx, op, a_bits, b_bits) with
/// operands in kA/kB. Binds every placeholder in `fallbacks` here, stores
/// the result or raises `err_id`.
void call_generic(TemplateBuilder& b, std::initializer_list<size_t> fallbacks,
                  uint32_t op_id, uint32_t err_id) {
    for (size_t ph : fallbacks) b.bind(ph);
    b.asm_.mov_reg_reg(Reg::RDI, kCtx);
    // The helper's op id is a per-template constant — emitted directly, no
    // patch site (it never varies across instantiations of this stencil).
    b.asm_.mov_reg_imm32sx(Reg::RSI, static_cast<int32_t>(op_id));
    b.asm_.mov_reg_reg(Reg::RDX, kA);
    b.asm_.mov_reg_reg(Reg::RCX, kB);
    b.asm_.call_mem(Mem{kCtx, Reg::RSP, 0, kCtxGenericBinop});
    b.asm_.cmp_reg_imm32(kA, static_cast<int32_t>(kUndefinedRawBits));
    const size_t err_jcc = b.asm_.placeholder_jcc(CC_E);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(err_jcc);
    b.error_exit(err_id);
}

/// Branchy boolean store: materializes false, jumps over true, then true.
/// Returns the FALSE-block offset so sibling failure paths (unordered float
/// compares, guard misses) can share it instead of duplicating the store.
size_t store_boolean(TemplateBuilder& b, uint8_t cc_true) {
    const size_t t = b.asm_.placeholder_jcc(cc_true);
    const size_t false_block = b.here();
    b.asm_.mov_reg_imm32sx(kA, kBitsFalse);
    const size_t jf = b.asm_.placeholder_jmp();
    b.bind(t);
    b.asm_.mov_reg_imm32sx(kA, kBitsTrue);
    b.bind(jf);
    b.store_vreg(kA, VR_Dst);
    return false_block;
}

/// Extracts the heap-object base: kA (tagged bits) -> kA = untagged base.
/// The tag test checks the full low nibble — immediates (null/undefined/
/// booleans) share bit0 == 1 with heap pointers, so bit0 alone is unsound.
/// Returns the failure jcc placeholder for the caller to bind.
size_t extract_heap_obj(TemplateBuilder& b) {
    b.asm_.mov_reg_reg(kC, kA);
    b.asm_.and_reg_imm32(kC, kTagMask);
    b.asm_.cmp_reg_imm32(kC, kTagHeapBits);
    const size_t jcc = b.asm_.placeholder_jcc(CC_NE);
    b.asm_.and_reg_imm32(kA, -16);
    return jcc;
}

/// Guard: kA is a boxed double. Untangles kA to the object base and compares
/// header.klass against the PA_KlassAddr imm64 site. Returns both failure
/// placeholders (tag + klass) for one shared bind point.
std::pair<size_t, size_t> guard_boxed_double(TemplateBuilder& b) {
    const size_t tag = extract_heap_obj(b);
    b.asm_.mov_reg_mem(kB, Mem{kA, Reg::RSP, 0, 0});  // header.klass
    b.imm64_site(kC, PatchKind::ConstantIndex, PA_KlassAddr);
    b.asm_.cmp_reg_reg(kB, kC);
    const size_t klass = b.asm_.placeholder_jcc(CC_NE);
    return {tag, klass};
}

/// Inline TLAB allocation of a boxed double: payload in xmm0 -> tagged
/// pointer in kA. Slow path calls alloc_slow(kind = double, payload bits).
/// Returns the OOM jcc placeholder — the CALLER binds it after its store +
/// tail and emits the error exit there (so success falls straight into the
/// store: fast path jumps forward over the slow block, slow success falls
/// through).
/// PERF (template contract): steady state = load top, add 24, cmp end,
/// 4 header/payload stores, tag-or — the T0 allocation fast path in native
/// form; BRANCHES: 1 (TLAB full, predicted not-taken).
size_t emit_alloc_double(TemplateBuilder& b) {
    b.asm_.mov_reg_mem(kC, Mem{kCtx, Reg::RSP, 0, kCtxTlabTop});
    b.asm_.mov_reg_reg(kB, kC);
    b.asm_.add_reg_imm32(kB, kBoxedDoubleAllocBytes);
    b.asm_.cmp_reg_mem(kB, Mem{kCtx, Reg::RSP, 0, kCtxTlabEnd});
    const size_t slow = b.asm_.placeholder_jcc(CC_A);
    b.asm_.mov_mem_reg(Mem{kCtx, Reg::RSP, 0, kCtxTlabTop}, kB);
    b.imm64_site(kA, PatchKind::ConstantIndex, PA_KlassAddr);  // double klass
    b.asm_.mov_mem_reg(Mem{kC, Reg::RSP, 0, 0}, kA);
    // header.size records the ROUNDED allocation (T0's heap walker walks
    // allocation strides, not logical payload sizes).
    b.asm_.mov_mem_imm32(Mem{kC, Reg::RSP, 0, 8}, kBoxedDoubleAllocBytes);
    b.asm_.mov_mem_imm32(Mem{kC, Reg::RSP, 0, 12}, 0);  // size + flags word
    b.asm_.movsd_mem_xmm(Mem{kC, Reg::RSP, 0, kObjectHeaderBytes}, Xmm::XMM0);
    b.asm_.mov_reg_reg(kA, kC);
    b.asm_.or_reg_imm8(kA, 1);
    const size_t join = b.asm_.placeholder_jmp();
    b.bind(slow);
    b.asm_.mov_reg_reg(Reg::RDI, kCtx);
    b.asm_.mov_reg_imm32sx(Reg::RSI, static_cast<int32_t>(kAllocDouble));
    b.asm_.movq_gpr_xmm(Reg::RDX, Xmm::XMM0);  // payload bits as arg a
    b.asm_.xor_reg_reg(Reg::RCX, Reg::RCX);
    b.asm_.call_mem(Mem{kCtx, Reg::RSP, 0, kCtxAllocSlow});
    b.asm_.test_reg_imm32(kA, -1);
    const size_t oom = b.asm_.placeholder_jcc(CC_E);
    b.asm_.or_reg_imm8(kA, 1);
    b.bind(join);
    return oom;
}

// ---- template emitters ----------------------------------------------------------

Stencil t_const_immediate(uint16_t op, int32_t bits) {
    TemplateBuilder b(op, false);
    // REX.W C7 /0: REX(1) opcode(1) modrm(1) disp32(4) imm32(4).
    b.asm_.mov_mem_imm32sx(Mem{Reg::RBP, Reg::RSP, 0, kTemplateVregDisp},
                           bits);
    b.mark(PatchKind::VirtualRegister, VR_Dst, 3);
    b.tail(BT_Next);
    return b.finish();
}

Stencil t_const_i32() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::CONST_I32), false);
    b.imm64_site(kA, PatchKind::ConstantIndex, PA_ConstSmi);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    return b.finish();
}

Stencil t_const_pool(uint16_t op) {
    // Load the materialized constant: rax = ctx->constants[8 * meta].
    TemplateBuilder b(op, false);
    b.asm_.mov_reg_mem(kA, Mem{kCtx, Reg::RSP, 0, kCtxConstants});
    const size_t off = b.here() + 3;  // REX + 8B + modrm, then disp32
    b.asm_.mov_reg_mem(kA, Mem{kA, Reg::RSP, 0, 0});
    b.mark(PatchKind::ConstantIndex, PA_ConstPoolOff, off);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    return b.finish();
}

Stencil t_move() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::MOVE), false);
    b.load_vreg(kA, VR_Src0);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    return b.finish();
}

// Typed smi arithmetic with in-template canonical fallback (mirrors T0's
// typed handlers: non-smi or overflow -> generic path -> T0's error).
enum class SmiOp { Add, Sub, Mul };

Stencil t_smi_binop(ugb::Op op, SmiOp kind, uint32_t fallback_op_id,
                     uint32_t err_id) {
    TemplateBuilder b(static_cast<uint16_t>(op), true);
    b.load_vreg(kA, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    const size_t guard = smi_guard(b);
    if (kind == SmiOp::Mul) {
        // Multiplication cannot ride the tagged encoding: unbox, imul, and
        // range-check explicitly. `jo` alone only catches int64 overflow —
        // T0 also traps for products inside int64 but outside the int63 Smi
        // range (2^62 <= |p| < 2^63), so an explicit bound compare follows.
        b.asm_.mov_reg_reg(kD, kA);
        b.asm_.sar_reg_imm8(kD, 1);
        b.asm_.sar_reg_imm8(kB, 1);
        b.asm_.imul_reg_reg(kD, kB);
        const size_t ovf = b.asm_.placeholder_jcc(CC_O);
        b.asm_.mov_reg_imm64(kC, TaggedValue::smi_max());
        b.asm_.cmp_reg_reg(kD, kC);
        const size_t over = b.asm_.placeholder_jcc(CC_G);
        b.asm_.mov_reg_imm64(kC, TaggedValue::smi_min());
        b.asm_.cmp_reg_reg(kD, kC);
        const size_t under = b.asm_.placeholder_jcc(CC_L);
        b.asm_.shl_reg_imm8(kD, 1);
        b.store_vreg(kD, VR_Dst);
        b.tail(BT_Next);
        b.bind(guard);
        b.bind(ovf);
        b.bind(over);
        b.bind(under);
        b.load_vreg(kB, VR_Src1);  // kB was unboxed in place: reload bits
        call_generic(b, {}, fallback_op_id, err_id);
        return b.finish();
    }
    // Add/Sub ride the TAGGED encoding: (a<<1) + (b<<1) == (a+b)<<1, and the
    // int64 overflow flag on the tagged sum fires exactly when a+b leaves the
    // int63 Smi range — the hardware check IS T0's range check (T0: builtin
    // overflow + explicit smi bounds; the tagged form subsumes both).
    b.asm_.mov_reg_reg(kD, kA);
    switch (kind) {
    case SmiOp::Add: b.asm_.add_reg_reg(kD, kB); break;
    case SmiOp::Sub: b.asm_.sub_reg_reg(kD, kB); break;
    case SmiOp::Mul: break;
    }
    const size_t ovf = b.asm_.placeholder_jcc(CC_O);
    b.store_vreg(kD, VR_Dst);
    b.tail(BT_Next);
    b.bind(guard);
    b.bind(ovf);
    b.load_vreg(kB, VR_Src1);  // kB is scratch-shared with the guard: reload
    call_generic(b, {}, fallback_op_id, err_id);
    return b.finish();
}

// Checked form: overflow is a defined trap, never a fallback (T0's
// *_CHECKED_* semantics). Non-smi -> typed error, overflow -> typed error.
Stencil t_smi_checked(ugb::Op op, SmiOp, uint32_t err_id) {
    TemplateBuilder b(static_cast<uint16_t>(op), true);
    b.load_vreg(kA, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    const size_t guard = smi_guard(b);
    // Tagged add/sub: the overflow flag IS the Smi-range trap (see
    // t_smi_binop). The checked form traps instead of falling back.
    b.asm_.add_reg_reg(kA, kB);
    const size_t ovf = b.asm_.placeholder_jcc(CC_O);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(guard);
    b.bind(ovf);
    b.error_exit(err_id);
    return b.finish();
}

// Canonical (generic) arithmetic: always the helper — smi-only semantics
// with T0's exact overflow/type error split implemented helper-side.
Stencil t_generic_binop(ugb::Op op, uint32_t err_id) {
    TemplateBuilder b(static_cast<uint16_t>(op), false);
    b.load_vreg(kA, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    call_generic(b, {}, static_cast<uint32_t>(op), err_id);
    return b.finish();
}

// I64 add/sub: smi operands, tagged add with overflow trap
// ("Add.I64: overflow or non-integer operand").
Stencil t_i64_binop(ugb::Op op, SmiOp kind, uint32_t err_id) {
    TemplateBuilder b(static_cast<uint16_t>(op), false);
    b.load_vreg(kA, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    const size_t guard = smi_guard(b);
    // Tagged add/sub: the overflow flag IS the "Add.I64: overflow" trap.
    if (kind == SmiOp::Sub) {
        b.asm_.sub_reg_reg(kA, kB);
    } else {
        b.asm_.add_reg_reg(kA, kB);
    }
    const size_t ovf = b.asm_.placeholder_jcc(CC_O);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(guard);
    b.bind(ovf);
    b.error_exit(err_id);
    return b.finish();
}

// Float64 binop: both operands must be boxed doubles (T0's as_double
// contract — Smis are rejected), payload op, TLAB-allocated boxed result.
Stencil t_f64_binop(ugb::Op op, uint8_t sse_op /*assembler method selector*/,
                     uint32_t err_id) {
    // sse_op: 0=add 1=sub 2=mul 3=div (dispatch below keeps the emitter one
    // function; the selector is build-time only, never a runtime switch).
    TemplateBuilder b(static_cast<uint16_t>(op), false);
    b.load_vreg(kD, VR_Src0);  // kD keeps src0's bits for the error path
    b.asm_.mov_reg_reg(kA, kD);
    const auto [tag0, klass0] = guard_boxed_double(b);
    b.asm_.movsd_xmm_mem(Xmm::XMM0, Mem{kA, Reg::RSP, 0, kObjectHeaderBytes});
    b.load_vreg(kD, VR_Src1);
    b.asm_.mov_reg_reg(kA, kD);
    const auto [tag1, klass1] = guard_boxed_double(b);
    b.asm_.movsd_xmm_mem(Xmm::XMM1, Mem{kA, Reg::RSP, 0, kObjectHeaderBytes});
    switch (sse_op) {
    case 0: b.asm_.addsd(Xmm::XMM0, Xmm::XMM1); break;
    case 1: b.asm_.subsd(Xmm::XMM0, Xmm::XMM1); break;
    case 2: b.asm_.mulsd(Xmm::XMM0, Xmm::XMM1); break;
    default: b.asm_.divsd(Xmm::XMM0, Xmm::XMM1); break;  // IEEE div-zero
    }
    const size_t oom = emit_alloc_double(b);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(tag0);
    b.bind(klass0);
    b.bind(tag1);
    b.bind(klass1);
    b.error_exit(err_id);
    b.bind(oom);
    b.error_exit(static_cast<uint32_t>(E::kErrAllocOOM));
    return b.finish();
}

Stencil t_neg_i64() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::NEG_I64), false);
    b.load_vreg(kA, VR_Src0);
    b.asm_.test_reg_imm8(kA, 1);
    const size_t guard = b.asm_.placeholder_jcc(CC_E);  // ZF=1 -> Smi
    b.asm_.sar_reg_imm8(kA, 1);
    b.asm_.mov_reg_imm64(kB, TaggedValue::smi_min());
    b.asm_.cmp_reg_reg(kA, kB);
    const size_t ovf = b.asm_.placeholder_jcc(CC_E);
    b.asm_.neg_reg(kA);
    b.asm_.shl_reg_imm8(kA, 1);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(guard);
    b.bind(ovf);
    b.error_exit(static_cast<uint32_t>(E::kErrNegI));
    return b.finish();
}

Stencil t_neg_f64() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::NEG_F64), false);
    b.load_vreg(kA, VR_Src0);
    const auto [tag, klass] = guard_boxed_double(b);
    b.asm_.movsd_xmm_mem(Xmm::XMM0, Mem{kA, Reg::RSP, 0, kObjectHeaderBytes});
    b.asm_.pxor_xmm_xmm(Xmm::XMM1, Xmm::XMM1);
    b.asm_.subsd(Xmm::XMM1, Xmm::XMM0);  // 0 - x (IEEE sign flip)
    b.asm_.movsd_xmm_xmm(Xmm::XMM0, Xmm::XMM1);
    const size_t oom = emit_alloc_double(b);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(tag);
    b.bind(klass);
    b.error_exit(static_cast<uint32_t>(E::kErrNegF64));
    b.bind(oom);
    b.error_exit(static_cast<uint32_t>(E::kErrAllocOOM));
    return b.finish();
}

// Division/remainder via the helper (the assembler has no idiv; the helper
// implements T0's exact zero + smi_min/-1 semantics).
Stencil t_div_rem(ugb::Op op, uint32_t err_id) {
    TemplateBuilder b(static_cast<uint16_t>(op), false);
    b.load_vreg(kA, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    call_generic(b, {}, static_cast<uint32_t>(op), err_id);
    return b.finish();
}

// Smi comparison family (T0: generic_compare — smi-only, boolean result).
Stencil t_cmp_smi(ugb::Op op, uint8_t cc) {
    TemplateBuilder b(static_cast<uint16_t>(op), false);
    b.load_vreg(kA, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    const size_t guard = smi_guard(b);
    b.asm_.sar_reg_imm8(kA, 1);
    b.asm_.sar_reg_imm8(kB, 1);
    b.asm_.cmp_reg_reg(kA, kB);
    const size_t cont = b.asm_.placeholder_jmp();
    b.asm_.bind_placeholder(guard);
    // Non-smi: T0's generic_compare raises a type error.
    b.error_exit(static_cast<uint32_t>(E::kErrCompare));
    b.asm_.bind_placeholder(cont);
    store_boolean(b, cc);
    b.tail(BT_Next);
    return b.finish();
}

Stencil t_eq_ref(bool negate) {
    TemplateBuilder b(
        static_cast<uint16_t>(negate ? ugb::Op::NE_REF : ugb::Op::EQ_REF),
        false);
    b.load_vreg(kA, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    b.asm_.cmp_reg_reg(kA, kB);
    store_boolean(b, negate ? CC_NE : CC_E);
    b.tail(BT_Next);
    return b.finish();
}

Stencil t_eq_null() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::EQ_NULL), false);
    b.load_vreg(kA, VR_Src0);
    b.asm_.cmp_reg_imm32(kA, kBitsNull);
    store_boolean(b, CC_E);
    b.tail(BT_Next);
    return b.finish();
}

Stencil t_eq_any() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::EQ_ANY), false);
    b.load_vreg(kA, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    call_generic(b, {}, static_cast<uint32_t>(ugb::Op::EQ_I32),
                 static_cast<uint32_t>(E::kErrCompare));
    return b.finish();
}

// Float64 equality: IEEE — unordered (NaN) compares false.
Stencil t_eq_f64() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::EQ_F64), false);
    b.load_vreg(kD, VR_Src0);
    b.asm_.mov_reg_reg(kA, kD);
    const auto [tag0, klass0] = guard_boxed_double(b);
    b.asm_.movsd_xmm_mem(Xmm::XMM0, Mem{kA, Reg::RSP, 0, kObjectHeaderBytes});
    b.load_vreg(kD, VR_Src1);
    b.asm_.mov_reg_reg(kA, kD);
    const auto [tag1, klass1] = guard_boxed_double(b);
    b.asm_.movsd_xmm_mem(Xmm::XMM1, Mem{kA, Reg::RSP, 0, kObjectHeaderBytes});
    b.asm_.ucomisd(Xmm::XMM0, Xmm::XMM1);
    // All flags consumers read the SAME ucomisd flags: not-equal and
    // unordered share the false materialization of store_boolean.
    const size_t ne = b.asm_.placeholder_jcc(CC_NE);
    const size_t unordered = b.asm_.placeholder_jcc(CC_P);
    const size_t false_block = store_boolean(b, CC_E);
    b.bind_at(ne, false_block);
    b.bind_at(unordered, false_block);
    b.tail(BT_Next);
    b.bind(tag0);
    b.bind(klass0);
    b.bind(tag1);
    b.bind(klass1);
    b.error_exit(static_cast<uint32_t>(E::kErrEqF64));
    return b.finish();
}

// Float64 ordered comparison: unordered is false for all four predicates.
Stencil t_cmp_f64(ugb::Op op, uint8_t cc) {
    TemplateBuilder b(static_cast<uint16_t>(op), false);
    b.load_vreg(kD, VR_Src0);
    b.asm_.mov_reg_reg(kA, kD);
    const auto [tag0, klass0] = guard_boxed_double(b);
    b.asm_.movsd_xmm_mem(Xmm::XMM0, Mem{kA, Reg::RSP, 0, kObjectHeaderBytes});
    b.load_vreg(kD, VR_Src1);
    b.asm_.mov_reg_reg(kA, kD);
    const auto [tag1, klass1] = guard_boxed_double(b);
    b.asm_.movsd_xmm_mem(Xmm::XMM1, Mem{kA, Reg::RSP, 0, kObjectHeaderBytes});
    b.asm_.ucomisd(Xmm::XMM0, Xmm::XMM1);
    const size_t unordered = b.asm_.placeholder_jcc(CC_P);
    const size_t cont = b.asm_.placeholder_jmp();
    b.bind(unordered);
    b.asm_.mov_reg_imm32sx(kA, kBitsFalse);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.asm_.bind_placeholder(cont);
    store_boolean(b, cc);
    b.tail(BT_Next);
    b.bind(tag0);
    b.bind(klass0);
    b.bind(tag1);
    b.bind(klass1);
    b.error_exit(static_cast<uint32_t>(E::kErrCmpF64));
    return b.finish();
}

Stencil t_i64_to_f64() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::I64_TO_F64), false);
    b.load_vreg(kA, VR_Src0);
    b.asm_.test_reg_imm8(kA, 1);
    const size_t guard = b.asm_.placeholder_jcc(CC_E);  // Smi -> ZF=1
    b.asm_.sar_reg_imm8(kA, 1);
    b.asm_.cvtsi2sd(Xmm::XMM0, kA);
    const size_t oom = emit_alloc_double(b);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(guard);
    b.error_exit(static_cast<uint32_t>(E::kErrI64ToF64));
    b.bind(oom);
    b.error_exit(static_cast<uint32_t>(E::kErrAllocOOM));
    return b.finish();
}

// F64 -> I64 saturating conversion (NaN -> 0, clamp to Smi bounds) runs in
// the generic helper — the inline sequence needs double-boundary compares
// that do not justify their size on a baseline tier.
Stencil t_f64_to_i64() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::F64_TO_I64), false);
    b.load_vreg(kA, VR_Src0);
    const auto [tag, klass] = guard_boxed_double(b);
    // The helper takes the ORIGINAL tagged bits as arg `a` (rdx): load
    // DIRECTLY into rdx — kD (rsi) carries the op id below and must not be
    // the value carrier (a prior version clobbered it with the op id).
    b.load_vreg(Reg::RDX, VR_Src0);
    b.asm_.mov_reg_reg(Reg::RDI, kCtx);
    // Per-template constant op id — emitted directly, never patched.
    b.asm_.mov_reg_imm32sx(Reg::RSI,
                           static_cast<int32_t>(ugb::Op::F64_TO_I64));
    b.asm_.xor_reg_reg(Reg::RCX, Reg::RCX);
    b.asm_.call_mem(Mem{kCtx, Reg::RSP, 0, kCtxGenericBinop});
    b.asm_.cmp_reg_imm32(kA, static_cast<int32_t>(kUndefinedRawBits));
    const size_t err = b.asm_.placeholder_jcc(CC_E);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(err);
    b.error_exit(static_cast<uint32_t>(E::kErrF64ToI64));
    b.bind(tag);
    b.bind(klass);
    b.error_exit(static_cast<uint32_t>(E::kErrF64ToI64));
    return b.finish();
}


// ---- control flow -------------------------------------------------------------

Stencil t_jump() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::JUMP), false);
    b.tail(BT_Instr);  // the continuation IS the branch target
    return b.finish();
}

// Truthiness inline (TaggedValue::truthy): Smi -> payload != 0; the
// false-ish immediates (false/null/undefined) -> false; everything else
// (true, heap objects) -> true. `sense` selects JUMP_TRUE / JUMP_FALSE.
Stencil t_jump_cond(ugb::Op op, bool jump_on_truthy) {
    TemplateBuilder b(static_cast<uint16_t>(op), false);
    const uint8_t cc_taken = jump_on_truthy ? CC_NE : CC_E;
    b.load_vreg(kA, VR_Src0);
    b.asm_.test_reg_imm8(kA, 1);
    const size_t to_smi = b.asm_.placeholder_jcc(CC_E);  // ZF=1 -> Smi
    // Non-smi path: the three false-ish immediates.
    b.asm_.cmp_reg_imm32(kA, kBitsFalse);
    const size_t j_false1 = b.asm_.placeholder_jcc(CC_E);
    b.asm_.cmp_reg_imm32(kA, kBitsNull);
    const size_t j_false2 = b.asm_.placeholder_jcc(CC_E);
    b.asm_.cmp_reg_imm32(kA, kBitsUndefined);
    const size_t j_false3 = b.asm_.placeholder_jcc(CC_E);
    // Truthy here. JUMP_TRUE: taken; JUMP_FALSE: fall to not-taken.
    const size_t truthy = b.asm_.placeholder_jmp();
    b.bind(to_smi);
    // Smi: payload != 0 is truthy (sign-insensitive: != 0 covers negatives).
    b.asm_.test_reg_imm32(kA, -1);
    const size_t smi_taken = b.asm_.placeholder_jcc(cc_taken);
    const size_t to_notaken = b.asm_.placeholder_jmp();
    // False-ish immediates land here (jump_on_truthy -> not taken).
    b.bind(j_false1);
    b.bind(j_false2);
    b.bind(j_false3);
    const size_t imm_notaken = b.asm_.placeholder_jmp();
    // (jump_on_truthy: immediates are false -> not-taken; else -> taken.)
    const size_t notaken = b.here();
    b.tail(BT_Next);
    const size_t taken = b.here();
    b.tail(BT_Instr);
    // Wire the jumps by sense.
    if (jump_on_truthy) {
        b.bind_at(truthy, taken);
        b.bind_at(imm_notaken, notaken);
    } else {
        b.bind_at(truthy, notaken);
        b.bind_at(imm_notaken, taken);
    }
    b.bind_at(smi_taken, taken);
    b.bind_at(to_notaken, notaken);
    return b.finish();
}

// JUMP_EQ / JUMP_NE: smi compare fused with the branch (T0: compare then
// branch on the boolean; non-smi compare operands are a type error).
Stencil t_jump_cmp(ugb::Op op, bool equal) {
    TemplateBuilder b(static_cast<uint16_t>(op), false);
    b.load_vreg(kA, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    const size_t guard = smi_guard(b);
    b.asm_.sar_reg_imm8(kA, 1);
    b.asm_.sar_reg_imm8(kB, 1);
    b.asm_.cmp_reg_reg(kA, kB);
    const size_t taken = b.asm_.placeholder_jcc(equal ? CC_E : CC_NE);
    b.tail(BT_Next);
    b.bind(taken);
    b.tail(BT_Instr);
    b.bind(guard);
    b.error_exit(static_cast<uint32_t>(E::kErrCompare));
    return b.finish();
}

Stencil t_return() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::RETURN), false);
    b.load_vreg(kA, VR_Src0);
    b.asm_.mov_mem_reg(Mem{kRetPtr, Reg::RSP, 0, 0}, kA);
    b.asm_.xor_reg_reg(kA, kA);
    b.tail(BT_Ok);
    return b.finish();
}

Stencil t_return_unit() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::RETURN_UNIT), false);
    b.asm_.mov_mem_imm32sx(Mem{kRetPtr, Reg::RSP, 0, 0}, kBitsUndefined);
    b.asm_.xor_reg_reg(kA, kA);
    b.tail(BT_Ok);
    return b.finish();
}

Stencil t_unreachable() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::UNREACHABLE), false);
    // No continuation tail: the error exit IS the only way out, so this
    // stencil is never a superstencil fusion candidate.
    b.error_exit(static_cast<uint32_t>(E::kErrUnreachable));
    return b.finish();
}

// ---- calls ----------------------------------------------------------------------

// Token-driven call: the helper resolves the callee (T0 fallback for M1 —
// direct-call patch points arrive with LDPT/J4 wiring, docs/ldpt.md 4).
// Args: src0 = argument window base vreg, src1 = argc vreg (Smi), dst =
// result vreg. The helper writes the result through the out pointer and
// returns 0, or returns the error id.
Stencil t_call(ugb::Op op) {
    const bool builtin = op == ugb::Op::CALL_BUILTIN;
    TemplateBuilder b(static_cast<uint16_t>(op), false);
    b.lea_vreg(Reg::RDX, 4 /* PA_ArgBase */);
    // argc is the LITERAL in the src1 field (T0: span(&R[s0], ins.s1)),
    // not a vreg — emitted as a patchable immediate.
    b.imm32sx_site(kB, PatchKind::ConstantIndex, PA_CallArgc);
    b.imm32sx_site(Reg::RSI, PatchKind::ConstantIndex, PA_CallToken);
    b.asm_.mov_reg_reg(Reg::RDI, kCtx);
    b.lea_vreg(Reg::R8, VR_Dst);  // out = &dst vreg
    b.asm_.call_mem(Mem{kCtx, Reg::RSP, 0,
                        builtin ? kCtxInvokeBuiltin : kCtxInvokeMethod});
    b.asm_.test_reg_imm32(kA, -1);
    const size_t raise = b.asm_.placeholder_jcc(CC_NE);
    b.tail(BT_Next);
    b.bind(raise);
    b.tail(BT_Error);  // rax already carries the helper's error id
    return b.finish();
}

// ---- allocation -----------------------------------------------------------------

// NEW_OBJECT: inline TLAB fast path (header + zeroed fields) with the
// alloc_slow helper on TLAB miss. PERF (template contract): steady state =
// size fold + top/end compare + header stores; BRANCHES: 1 (miss).
Stencil t_new_object() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::NEW_OBJECT), false);
    b.imm32sx_site(kC, PatchKind::ConstantIndex, PA_FieldCount);
    b.asm_.mov_reg_reg(kD, kC);
    b.asm_.shl_reg_imm8(kD, 3);  // 8 bytes per tagged field
    b.asm_.add_reg_imm32(kD, kObjectHeaderBytes);
    b.asm_.add_reg_imm32(kD, 15);  // TLAB alignment round-up
    b.asm_.and_reg_imm32(kD, -16);
    b.asm_.mov_reg_mem(kA, Mem{kCtx, Reg::RSP, 0, kCtxTlabTop});
    b.asm_.mov_reg_reg(kB, kA);
    b.asm_.add_reg_reg(kB, kD);
    b.asm_.cmp_reg_mem(kB, Mem{kCtx, Reg::RSP, 0, kCtxTlabEnd});
    const size_t slow = b.asm_.placeholder_jcc(CC_A);
    b.asm_.mov_mem_reg(Mem{kCtx, Reg::RSP, 0, kCtxTlabTop}, kB);
    b.imm64_site(kB, PatchKind::ConstantIndex, PA_KlassAddr);
    b.asm_.mov_mem_reg(Mem{kA, Reg::RSP, 0, 0}, kB);      // header.klass
    b.asm_.mov_mem_reg(Mem{kA, Reg::RSP, 0, 8}, kD);      // size + zero flags
    b.asm_.mov_reg_reg(kD, kA);
    b.asm_.add_reg_imm32(kD, kObjectHeaderBytes);         // fields walk
    b.asm_.cmp_reg_imm32(kC, 0);
    const size_t no_fields = b.asm_.placeholder_jcc(CC_E);
    const size_t zero_top = b.here();
    // T0's allocator zero-fills fields to NULL (not Smi 0) — the tagged
    // null immediate keeps EQ_NULL/truthiness parity for uninitialized
    // fields (icggc.cpp allocate_object).
    b.asm_.mov_mem_imm32sx(Mem{kD, Reg::RSP, 0, 0}, kBitsNull);
    b.asm_.add_reg_imm32(kD, 8);
    b.asm_.dec_reg(kC);
    const size_t zero_jcc = b.asm_.placeholder_jcc(CC_NE);
    b.bind_backward(zero_jcc, zero_top);
    b.bind(no_fields);
    b.asm_.or_reg_imm8(kA, 1);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(slow);
    b.asm_.mov_reg_reg(Reg::RDI, kCtx);
    b.asm_.mov_reg_imm32sx(Reg::RSI, static_cast<int32_t>(kAllocObject));
    b.imm32sx_site(Reg::RDX, PatchKind::ConstantIndex, PA_KlassToken);
    b.imm32sx_site(Reg::RCX, PatchKind::ConstantIndex, PA_FieldCount);
    b.asm_.call_mem(Mem{kCtx, Reg::RSP, 0, kCtxAllocSlow});
    b.asm_.test_reg_imm32(kA, -1);
    const size_t oom = b.asm_.placeholder_jcc(CC_E);
    b.asm_.or_reg_imm8(kA, 1);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(oom);
    b.error_exit(static_cast<uint32_t>(E::kErrAllocOOM));
    return b.finish();
}

// NEW_ARRAY: smi length guard (0 <= len <= max), inline TLAB bump with a
// variable size, zeroed elements. Arrays carry klass == 0 in the header —
// that is the array discriminator every ARRAY_* stencil tests.
Stencil t_new_array() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::NEW_ARRAY), false);
    b.load_vreg(kA, VR_Src0);
    b.asm_.test_reg_imm8(kA, 1);
    const size_t bad = b.asm_.placeholder_jcc(CC_NE);  // non-Smi length
    b.asm_.mov_reg_reg(kB, kA);
    b.asm_.sar_reg_imm8(kB, 1);
    b.asm_.cmp_reg_imm32(kB, 0);
    const size_t neg = b.asm_.placeholder_jcc(CC_L);
    b.imm32sx_site(kC, PatchKind::ConstantIndex, PA_MaxArrayLen);
    b.asm_.cmp_reg_reg(kB, kC);
    const size_t too_big = b.asm_.placeholder_jcc(CC_G);
    b.asm_.mov_reg_reg(kD, kB);
    b.asm_.shl_reg_imm8(kD, 3);
    b.asm_.add_reg_imm32(kD, kArrayHeaderBytes);
    b.asm_.add_reg_imm32(kD, 15);  // TLAB alignment round-up
    b.asm_.and_reg_imm32(kD, -16);
    b.asm_.mov_reg_mem(kE, Mem{kCtx, Reg::RSP, 0, kCtxTlabTop});
    b.asm_.mov_reg_reg(kF, kE);
    b.asm_.add_reg_reg(kF, kD);
    b.asm_.cmp_reg_mem(kF, Mem{kCtx, Reg::RSP, 0, kCtxTlabEnd});
    const size_t slow = b.asm_.placeholder_jcc(CC_A);
    b.asm_.mov_mem_reg(Mem{kCtx, Reg::RSP, 0, kCtxTlabTop}, kF);
    b.asm_.mov_mem_imm32sx(Mem{kE, Reg::RSP, 0, 0}, 0);  // klass = 0 (array)
    b.asm_.mov_mem_reg(Mem{kE, Reg::RSP, 0, 8}, kD);     // size + zero flags
    b.asm_.mov_mem_reg(Mem{kE, Reg::RSP, 0, 16}, kB);    // length + zero pad
    b.asm_.mov_reg_reg(kC, kE);
    b.asm_.add_reg_imm32(kC, kArrayHeaderBytes);         // elements walk
    b.asm_.cmp_reg_imm32(kB, 0);
    const size_t no_elems = b.asm_.placeholder_jcc(CC_E);
    const size_t zero_top = b.here();
    b.asm_.mov_mem_imm32sx(Mem{kC, Reg::RSP, 0, 0}, kBitsNull);
    b.asm_.add_reg_imm32(kC, 8);
    b.asm_.dec_reg(kB);
    const size_t zero_jcc = b.asm_.placeholder_jcc(CC_NE);
    b.bind_backward(zero_jcc, zero_top);
    b.bind(no_elems);
    b.asm_.mov_reg_reg(kA, kE);
    b.asm_.or_reg_imm8(kA, 1);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(slow);
    b.asm_.mov_reg_reg(Reg::RDI, kCtx);
    b.asm_.mov_reg_imm32sx(Reg::RSI, static_cast<int32_t>(kAllocArray));
    b.load_vreg(Reg::RDX, VR_Src0);
    b.asm_.sar_reg_imm8(Reg::RDX, 1);
    b.asm_.xor_reg_reg(Reg::RCX, Reg::RCX);
    b.asm_.call_mem(Mem{kCtx, Reg::RSP, 0, kCtxAllocSlow});
    b.asm_.test_reg_imm32(kA, -1);
    const size_t oom = b.asm_.placeholder_jcc(CC_E);
    b.asm_.or_reg_imm8(kA, 1);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(oom);
    b.error_exit(static_cast<uint32_t>(E::kErrAllocOOM));
    b.bind(bad);
    b.bind(neg);
    b.bind(too_big);
    b.error_exit(static_cast<uint32_t>(E::kErrNewArrayLen));
    return b.finish();
}

// ---- field access (IC-guarded) ----------------------------------------------------

// GET_FIELD: mono inline-cache guard (patchable — see patch_ic_guard),
// direct slot load on hit, get_field_slow helper otherwise. The guard
// compares header.klass against the PA_KlassAddr immediate; unpatched, the
// always-slow `jmp` routes every execution through the helper (correct for
// any receiver; strengthened by the instantiation pass).
Stencil t_get_field(uint16_t op) {
    TemplateBuilder b(op, false);
    b.load_vreg(kD, VR_Src0);  // receiver bits (kept for the helper)
    b.asm_.mov_reg_reg(kA, kD);
    const size_t tag = extract_heap_obj(b);
    b.asm_.mov_reg_mem(kB, Mem{kA, Reg::RSP, 0, 0});  // header.klass
    b.imm64_site(kC, PatchKind::ConstantIndex, PA_KlassAddr);
    b.asm_.cmp_reg_reg(kB, kC);
    // Always-slow slot: 0x90 0xE9 rel32; patch_ic_guard flips it to a
    // guarded 0F 85 rel32 once the compare immediates are live.
    b.asm_.nop(1);
    const size_t slow_rel = b.here() + 1;
    b.asm_.jmp_rel32(0);
    b.mark(PatchKind::IcSlot, 1, slow_rel);
    // Hit path: direct slot load (offset = 16 + 8*slot, patchable).
    const size_t field_off = b.here() + 3;
    b.asm_.mov_reg_mem(kC, Mem{kA, Reg::RSP, 0, kObjectHeaderBytes});
    b.mark(PatchKind::ConstantIndex, PA_FieldByteOff, field_off);
    b.store_vreg(kC, VR_Dst);
    b.tail(BT_Next);
    // Slow body (helper) — the target of the always-slow jmp and the tag
    // failure. Recorded AFTER the hit path so its offset is the body.
    const size_t slow_off = b.here();
    b.bind(tag);
    b.asm_.mov_reg_reg(Reg::RDI, kCtx);
    b.asm_.mov_reg_reg(Reg::RSI, kD);
    b.imm32sx_site(Reg::RDX, PatchKind::ConstantIndex, PA_FieldToken);
    b.asm_.call_mem(Mem{kCtx, Reg::RSP, 0, kCtxGetFieldSlow});
    b.asm_.test_reg_imm32(kA, -1);
    const size_t err = b.asm_.placeholder_jcc(CC_E);
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(err);
    b.error_exit(static_cast<uint32_t>(E::kErrGetField));
    b.out.slow_path_offset = static_cast<uint32_t>(slow_off);
    return b.finish();
}

// SET_FIELD: slot store + ICGGC card-marking barrier (docs/ldpt.md 4B uses
// the same inline shape). Only reference values mark the card.
// PERF (barrier contract): tag test (predicted not-reference for primitive
// stores) + heap-base fold + byte store; two predictable branches.
Stencil t_set_field(uint16_t op) {
    TemplateBuilder b(op, false);
    b.load_vreg(kD, VR_Src0);            // receiver bits
    b.load_vreg(kE, VR_Src1);            // value bits
    b.asm_.mov_reg_reg(kA, kD);
    const size_t tag = extract_heap_obj(b);
    b.asm_.mov_reg_mem(kB, Mem{kA, Reg::RSP, 0, 0});
    b.imm64_site(kC, PatchKind::ConstantIndex, PA_KlassAddr);
    b.asm_.cmp_reg_reg(kB, kC);
    b.asm_.nop(1);
    const size_t slow_rel = b.here() + 1;
    b.asm_.jmp_rel32(0);
    b.mark(PatchKind::IcSlot, 1, slow_rel);
    const size_t field_off = b.here() + 3;
    b.asm_.mov_mem_reg(Mem{kA, Reg::RSP, 0, kObjectHeaderBytes}, kE);
    b.mark(PatchKind::ConstantIndex, PA_FieldByteOff, field_off);
    // ---- card-marking barrier (ICGGC, kCardShift = 9) ----
    b.asm_.mov_reg_reg(kC, kE);
    b.asm_.and_reg_imm32(kC, kTagMask);
    b.asm_.cmp_reg_imm32(kC, kTagHeapBits);
    const size_t done = b.asm_.placeholder_jcc(CC_NE);  // not a reference
    b.asm_.mov_reg_mem(kC, Mem{kCtx, Reg::RSP, 0, kCtxHeapBase});
    b.asm_.sub_reg_reg(kA, kC);           // obj - heap_base
    b.asm_.shr_reg_imm8(kA, 9);           // >> kCardShift
    b.asm_.mov_reg_mem(kC, Mem{kCtx, Reg::RSP, 0, kCtxCardBase});
    b.asm_.mov_mem_imm8(Mem{kC, kA, 0, 0}, 1);  // card = DIRTY
    b.bind(done);
    b.tail(BT_Next);
    // Slow body (helper) — target of the always-slow jmp + tag failure.
    const size_t slow_off = b.here();
    b.bind(tag);
    b.asm_.mov_reg_reg(Reg::RDI, kCtx);
    b.asm_.mov_reg_reg(Reg::RSI, kD);
    b.asm_.mov_reg_reg(Reg::RDX, kE);
    b.imm32sx_site(Reg::RCX, PatchKind::ConstantIndex, PA_FieldToken);
    b.asm_.call_mem(Mem{kCtx, Reg::RSP, 0, kCtxSetFieldSlow});
    b.asm_.test_reg_imm32(kA, -1);
    const size_t err = b.asm_.placeholder_jcc(CC_E);
    b.tail(BT_Next);
    b.bind(err);
    b.error_exit(static_cast<uint32_t>(E::kErrSetField));
    b.out.slow_path_offset = static_cast<uint32_t>(slow_off);
    return b.finish();
}

// ---- arrays -----------------------------------------------------------------------

Stencil t_array_length() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::ARRAY_LENGTH), false);
    b.load_vreg(kA, VR_Src0);
    const size_t tag = extract_heap_obj(b);
    b.asm_.mov_reg_mem(kB, Mem{kA, Reg::RSP, 0, 0});
    b.asm_.test_reg_imm32(kB, -1);
    const size_t not_array = b.asm_.placeholder_jcc(CC_NE);  // klass != 0
    b.asm_.mov_reg32_mem(kA, Mem{kA, Reg::RSP, 0, 16});
    b.asm_.shl_reg_imm8(kA, 1);  // u32 length -> Smi
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(tag);
    b.bind(not_array);
    b.error_exit(static_cast<uint32_t>(E::kErrArrayLen));
    return b.finish();
}

// Shared bounds-checked element address: kA = array base, kB = unboxed
// index -> kC = &element. Guards: array discriminator + smi index + range.
struct ArrayGuards {
    size_t tag, not_array, not_smi, bounds;
};

ArrayGuards array_guards(TemplateBuilder& b) {
    ArrayGuards g;
    g.tag = extract_heap_obj(b);
    b.asm_.mov_reg_mem(kC, Mem{kA, Reg::RSP, 0, 0});
    b.asm_.test_reg_imm32(kC, -1);
    g.not_array = b.asm_.placeholder_jcc(CC_NE);
    b.asm_.test_reg_imm8(kB, 1);
    g.not_smi = b.asm_.placeholder_jcc(CC_NE);
    b.asm_.sar_reg_imm8(kB, 1);
    b.asm_.mov_reg32_mem(kC, Mem{kA, Reg::RSP, 0, 16});  // length
    b.asm_.cmp_reg_reg(kB, kC);
    g.bounds = b.asm_.placeholder_jcc(CC_AE);  // unsigned >= len
    b.asm_.lea_reg_mem(kC, Mem{kA, kB, 3, kArrayHeaderBytes});
    return g;
}

Stencil t_array_get() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::ARRAY_GET), false);
    b.load_vreg(kD, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    b.asm_.mov_reg_reg(kA, kD);
    const ArrayGuards g = array_guards(b);
    b.asm_.mov_reg_mem(kA, Mem{kC, Reg::RSP, 0, 0});
    b.store_vreg(kA, VR_Dst);
    b.tail(BT_Next);
    b.bind(g.tag);
    b.bind(g.not_array);
    b.bind(g.not_smi);
    b.error_exit(static_cast<uint32_t>(E::kErrArrayGet));
    b.bind(g.bounds);
    b.error_exit(static_cast<uint32_t>(E::kErrBounds));
    return b.finish();
}

Stencil t_array_set() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::ARRAY_SET), false);
    b.load_vreg(kD, VR_Src0);
    b.load_vreg(kB, VR_Src1);
    b.load_vreg(kE, VR_Src2);
    const size_t tag = extract_heap_obj(b);
    b.asm_.mov_reg_mem(kC, Mem{kA, Reg::RSP, 0, 0});
    b.asm_.test_reg_imm32(kC, -1);
    const size_t not_array = b.asm_.placeholder_jcc(CC_NE);
    b.asm_.test_reg_imm8(kB, 1);
    const size_t not_smi = b.asm_.placeholder_jcc(CC_NE);
    b.asm_.sar_reg_imm8(kB, 1);
    b.asm_.mov_reg32_mem(kC, Mem{kA, Reg::RSP, 0, 16});
    b.asm_.cmp_reg_reg(kB, kC);
    const size_t bounds = b.asm_.placeholder_jcc(CC_AE);
    b.asm_.lea_reg_mem(kC, Mem{kA, kB, 3, kArrayHeaderBytes});
    b.asm_.mov_mem_reg(Mem{kC, Reg::RSP, 0, 0}, kE);
    // Card barrier: only reference values mark (owner = the array).
    b.asm_.mov_reg_reg(kC, kE);
    b.asm_.and_reg_imm32(kC, kTagMask);
    b.asm_.cmp_reg_imm32(kC, kTagHeapBits);
    const size_t done = b.asm_.placeholder_jcc(CC_NE);
    b.asm_.mov_reg_mem(kC, Mem{kCtx, Reg::RSP, 0, kCtxHeapBase});
    b.asm_.sub_reg_reg(kA, kC);
    b.asm_.shr_reg_imm8(kA, 9);
    b.asm_.mov_reg_mem(kC, Mem{kCtx, Reg::RSP, 0, kCtxCardBase});
    b.asm_.mov_mem_imm8(Mem{kC, kA, 0, 0}, 1);
    b.bind(done);
    b.tail(BT_Next);
    b.bind(tag);
    b.bind(not_array);
    b.bind(not_smi);
    b.error_exit(static_cast<uint32_t>(E::kErrArraySet));
    b.bind(bounds);
    b.error_exit(static_cast<uint32_t>(E::kErrBounds));
    return b.finish();
}

// ---- guards -----------------------------------------------------------------------

Stencil t_check_null(bool expect_null) {
    const uint16_t op = static_cast<uint16_t>(
        expect_null ? ugb::Op::CHECK_NULL : ugb::Op::CHECK_NON_NULL);
    TemplateBuilder b(op, false);
    b.load_vreg(kA, VR_Src0);
    b.asm_.cmp_reg_imm32(kA, kBitsNull);
    const size_t fail = b.asm_.placeholder_jcc(expect_null ? CC_NE : CC_E);
    b.tail(BT_Next);
    b.bind(fail);
    b.error_exit(static_cast<uint32_t>(expect_null ? E::kErrCheckNull
                                                   : E::kErrCheckNonNull));
    return b.finish();
}

Stencil t_check_class() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::CHECK_CLASS), false);
    b.load_vreg(kD, VR_Src0);
    b.asm_.mov_reg_reg(kA, kD);
    const size_t tag = extract_heap_obj(b);
    b.asm_.mov_reg_mem(kB, Mem{kA, Reg::RSP, 0, 0});
    b.imm64_site(kC, PatchKind::ConstantIndex, PA_KlassAddr);
    b.asm_.cmp_reg_reg(kB, kC);
    const size_t fail = b.asm_.placeholder_jcc(CC_NE);
    b.tail(BT_Next);
    b.bind(tag);
    b.bind(fail);
    b.error_exit(static_cast<uint32_t>(E::kErrCheckClass));
    return b.finish();
}

Stencil t_safepoint_poll() {
    TemplateBuilder b(static_cast<uint16_t>(ugb::Op::SAFEPOINT_POLL), false);
    b.asm_.mov_reg_mem(kB, Mem{kCtx, Reg::RSP, 0, 0x40});  // safepoint_word
    b.asm_.mov_reg32_mem(kA, Mem{kB, Reg::RSP, 0, 0});
    b.asm_.test_reg_imm32(kA, -1);
    const size_t raise = b.asm_.placeholder_jcc(CC_NE);
    b.tail(BT_Next);
    b.bind(raise);
    // Armed: exit with the deopt id — the driver reruns the method in T0
    // from the entry with the original arguments (M1 suspension contract).
    b.error_exit(static_cast<uint32_t>(E::kErrDeopt));
    return b.finish();
}

}  // namespace

// ---- corpus assembly ---------------------------------------------------------------

size_t build_default_corpus(StencilTable& table) {
    size_t n = 0;
    const auto reg = [&](Stencil s) {
        table.register_stencil(std::move(s));
        ++n;
    };
    const auto op16 = [](ugb::Op o) { return static_cast<uint16_t>(o); };

    // Constants + moves.
    reg(t_const_immediate(op16(ugb::Op::CONST_NULL), kBitsNull));
    reg(t_const_immediate(op16(ugb::Op::CONST_UNDEFINED), kBitsUndefined));
    reg(t_const_immediate(op16(ugb::Op::CONST_FALSE), kBitsFalse));
    reg(t_const_immediate(op16(ugb::Op::CONST_TRUE), kBitsTrue));
    reg(t_const_i32());
    reg(t_const_pool(op16(ugb::Op::CONST_I64)));
    reg(t_const_pool(op16(ugb::Op::CONST_F64)));
    reg(t_const_pool(op16(ugb::Op::CONST_STRING)));
    reg(t_move());

    // Typed (speculative) smi arithmetic with in-template canonical fallback.
    reg(t_smi_binop(ugb::Op::ADD_I32, SmiOp::Add, op16(ugb::Op::ADD_ANY),
                    static_cast<uint32_t>(E::kErrAddTyped)));
    reg(t_smi_binop(ugb::Op::SUB_I32, SmiOp::Sub, op16(ugb::Op::SUB_ANY),
                    static_cast<uint32_t>(E::kErrSubTyped)));
    reg(t_smi_binop(ugb::Op::MUL_I32, SmiOp::Mul, op16(ugb::Op::MUL_ANY),
                    static_cast<uint32_t>(E::kErrMulTyped)));
    reg(t_smi_checked(ugb::Op::ADD_CHECKED_I32, SmiOp::Add,
                      static_cast<uint32_t>(E::kErrAddTyped)));

    // Canonical arithmetic (helper-backed, T0-exact semantics).
    reg(t_generic_binop(ugb::Op::ADD_ANY, static_cast<uint32_t>(E::kErrAddUnsupported)));
    reg(t_generic_binop(ugb::Op::SUB_ANY, static_cast<uint32_t>(E::kErrSubAny)));
    reg(t_generic_binop(ugb::Op::MUL_ANY, static_cast<uint32_t>(E::kErrMulAny)));
    reg(t_i64_binop(ugb::Op::ADD_I64, SmiOp::Add, static_cast<uint32_t>(E::kErrAddI64)));
    reg(t_i64_binop(ugb::Op::SUB_I64, SmiOp::Sub, static_cast<uint32_t>(E::kErrSubAny)));

    // Float64 arithmetic.
    reg(t_f64_binop(ugb::Op::ADD_F64, 0, static_cast<uint32_t>(E::kErrAddF64)));
    reg(t_f64_binop(ugb::Op::SUB_F64, 1, static_cast<uint32_t>(E::kErrSubF64)));
    reg(t_f64_binop(ugb::Op::MUL_F64, 2, static_cast<uint32_t>(E::kErrMulF64)));
    reg(t_f64_binop(ugb::Op::DIV_F64, 3, static_cast<uint32_t>(E::kErrDivF64)));
    reg(t_neg_i64());
    reg(t_neg_f64());
    reg(t_div_rem(ugb::Op::DIV_S_I64, static_cast<uint32_t>(E::kErrDivS)));
    reg(t_div_rem(ugb::Op::REM_S_I64, static_cast<uint32_t>(E::kErrRemS)));

    // Comparisons.
    reg(t_cmp_smi(ugb::Op::EQ_I32, CC_E));
    reg(t_cmp_smi(ugb::Op::EQ_I64, CC_E));
    reg(t_cmp_smi(ugb::Op::NE_I64, CC_NE));
    reg(t_cmp_smi(ugb::Op::LT_S_I64, CC_L));
    reg(t_cmp_smi(ugb::Op::LE_S_I64, CC_LE));
    reg(t_cmp_smi(ugb::Op::GT_S_I64, CC_G));
    reg(t_cmp_smi(ugb::Op::GE_S_I64, CC_GE));
    reg(t_eq_ref(false));
    reg(t_eq_ref(true));
    reg(t_eq_null());
    reg(t_eq_any());
    reg(t_eq_f64());
    reg(t_cmp_f64(ugb::Op::LT_F64, CC_B));
    reg(t_cmp_f64(ugb::Op::LE_F64, CC_BE));
    reg(t_cmp_f64(ugb::Op::GT_F64, CC_A));
    reg(t_cmp_f64(ugb::Op::GE_F64, CC_AE));

    // Conversions.
    reg(t_i64_to_f64());
    reg(t_f64_to_i64());

    // Control flow.
    reg(t_jump());
    reg(t_jump_cond(ugb::Op::JUMP_TRUE, true));
    reg(t_jump_cond(ugb::Op::JUMP_FALSE, false));
    reg(t_jump_cmp(ugb::Op::JUMP_EQ, true));
    reg(t_jump_cmp(ugb::Op::JUMP_NE, false));
    reg(t_return());
    reg(t_return_unit());
    reg(t_unreachable());

    // Calls.
    reg(t_call(ugb::Op::CALL_DIRECT));
    reg(t_call(ugb::Op::CALL_VIRTUAL));
    reg(t_call(ugb::Op::CALL_BUILTIN));

    // Allocation.
    reg(t_new_object());
    reg(t_new_array());

    // Fields (IC-guarded; speculative shape variants share the body).
    reg(t_get_field(op16(ugb::Op::GET_FIELD)));
    reg(t_get_field(op16(ugb::Op::GET_FIELD_SHAPE)));
    reg(t_set_field(op16(ugb::Op::SET_FIELD)));
    reg(t_set_field(op16(ugb::Op::SET_FIELD_SHAPE)));

    // Arrays.
    reg(t_array_length());
    reg(t_array_get());
    reg(t_array_set());

    // Guards + safepoint.
    reg(t_check_null(true));
    reg(t_check_null(false));
    reg(t_check_class());
    reg(t_safepoint_poll());

    return n;
}

// ---- superstencil promotion ---------------------------------------------------------

SuperstencilPromotionStats promote_superstencils(
    StencilTable& table, const std::unordered_map<uint64_t, uint64_t>& bigram_counts,
    uint32_t threshold) {
    SuperstencilPromotionStats stats;
    for (const auto& [key, count] : bigram_counts) {
        if (count < threshold) continue;
        ++stats.candidates;
        const uint16_t op1 = static_cast<uint16_t>(key >> 16);
        const uint16_t op2 = static_cast<uint16_t>(key & 0xFFFF);
        const Stencil* a = table.select(op1, false);
        if (a == nullptr) a = table.select(op1, true);
        const Stencil* b = table.select(op2, false);
        if (b == nullptr) b = table.select(op2, true);
        if (a == nullptr || b == nullptr) continue;
        // Fusion: a's BT_Next continuation tail (the ONLY site of that
        // kind) is nop-out in place and control falls through into b's
        // body. Helper-backed templates end with their error tail, so the
        // continuation is NOT necessarily the last site — searching by
        // operand keeps fusion valid for every stencil shape. a's tail
        // site REMAINS (it now routes to the fused continuation).
        const PatchSite* cont = nullptr;
        for (const PatchSite& ps : a->patch_sites) {
            if (ps.kind == PatchKind::BranchTarget &&
                ps.operand == BT_Next) {
                cont = &ps;
                break;
            }
        }
        // The nop-out only preserves semantics when the continuation tail is
        // the template's LAST 5 bytes: helper-backed templates end with
        // their error tail, and noping their mid-template continuation would
        // drop the success path into the error store. (The site marks the
        // rel32 at opcode+1, so the rel32's last byte is at size-1.)
        if (cont == nullptr ||
            cont->offset + 4 != a->bytes.size()) {
            continue;
        }
        const size_t a_len = a->bytes.size();
        Superstencil fused;
        fused.sequence = {op1, op2};
        fused.bytes = a->bytes;
        fused.bytes.insert(fused.bytes.end(), b->bytes.begin(),
                           b->bytes.end());
        static const uint8_t nop5[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
        std::memcpy(fused.bytes.data() + cont->offset, nop5, 5);
        fused.patch_sites = a->patch_sites;
        for (PatchSite ps : b->patch_sites) {
            ps.offset += static_cast<uint32_t>(a_len);
            fused.patch_sites.push_back(ps);
        }
        table.register_superstencil(std::move(fused));
        ++stats.promoted;
    }
    return stats;
}

// ---- IC guard strengthening -----------------------------------------------------------

void patch_ic_guard(uint8_t* code, const PatchSite& guard_imm,
                    const PatchSite& slow_rel, uint64_t klass) {
    // 1. Compare immediate: the expected klass pointer.
    std::memcpy(code + guard_imm.offset, &klass, sizeof(klass));
    // 2. Flip `nop; jmp rel32` (0x90 0xE9 cd) into `jne rel32` (0F 85 cd).
    //    The rel32 stays at the same bytes AND the next-ip is unchanged:
    //    the jmp starts at (offset-1) with next-ip (offset+4); the jne
    //    starts at (offset-2) with next-ip (offset+4). Same target, same
    //    displacement — the flip is a pure opcode-prefix rewrite.
    code[slow_rel.offset - 2] = 0x0F;
    code[slow_rel.offset - 1] = 0x85;
}

}  // namespace vortex::j1
