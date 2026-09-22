// J1 — no-IR stencil baseline JIT (docs/tier-j1.md, docs/roadmap.md M1).
//
// compile() walks the UGB instruction stream once to decode + select
// stencils and compute native offsets, then a second time to instantiate:
// memcpy the template and patch its sites. The shared prologue, the shared
// epilogue and the OSR entry stubs are emitted directly with the project's
// x86-64 assembler.
//
// The J1Context helpers below are the C++ side of the context function-
// pointer ABI (j1/context.hpp) and mirror T0's observable semantics exactly
// (parity DoD, docs/roadmap.md M1 — enforced by tests/test_j1.cpp).
//
// TLAB ownership under the single-mutator contract: J1-compiled code bumps
// its context's TLAB snapshot natively; every helper that can allocate or
// that re-enters T0 re-syncs the heap's view before touching the allocator
// and reads it back after (gc::Heap::sync_tlab_top). run_baseline() performs
// the final sync on the exit path.
//
// Frequency classes (CEM-26 section 3):
//   @warm — compile()/make_j1_bindings: once per method per tier-up.
//   @cold — the helpers: slow paths behind generated guards (TLAB refill,
//           unresolved fields, token calls). Reachable from hot code only
//           through the corpus's documented call sites.
#include "vortex/j1/baseline_jit.hpp"

#include <cstring>
#include <unordered_map>

#include "vortex/j1/stencil_corpus.hpp"
#include "vortex/ugb/encoding.hpp"

namespace vortex::j1 {

using support::fail;
using support::Result;
namespace x64 = vortex::codegen::x64;

namespace {

using x64::Mem;
using x64::Reg;

constexpr uint32_t kKindObject = 0;
constexpr uint32_t kKindArray = 1;
constexpr uint32_t kKindDouble = 2;

// Named bound mirroring InterpreterConfig::max_array_length (Rule 72): the
// corpus's PA_MaxArrayLen site patches to this value. Kept identical to the
// interpreter default so T0 and J1 accept the same array lengths.
constexpr uint32_t kJ1MaxArrayLength = vm::InterpreterConfig{}.max_array_length;

constexpr uint32_t kNoOsr = 0xFFFFFFFF;

uint64_t ptr_bits(const void* p) noexcept {
    return reinterpret_cast<uint64_t>(p);
}

// ---- TLAB sync discipline ------------------------------------------------------

int64_t helper_invoke_token(J1Context*, uint32_t, const TaggedValue*, uint32_t,
                            TaggedValue*) noexcept;

// Every helper that allocates or re-enters T0 adopts the context snapshot
// first and publishes the heap's view back afterwards.
void tlab_adopt(J1Context* ctx) noexcept {
    static_cast<gc::Heap*>(ctx->heap)->sync_tlab_top(
        static_cast<uint8_t*>(ctx->tlab_top));
}
void tlab_publish(J1Context* ctx) noexcept {
    ctx->tlab_top = static_cast<gc::Heap*>(ctx->heap)->tlab_top();
    ctx->tlab_end = static_cast<gc::Heap*>(ctx->heap)->tlab_end();
}

// ---- helpers (the C++ side of the J1Context ABI) ---------------------------------

// @cold — slow allocation behind the corpus's inline TLAB fast paths.
// PERF_CONTRACT:
// BUDGET: heap fast path <= 10 cycles + amortized refill; reached only on
//         the templates' TLAB-miss branch
// READS: heap metadata; WRITES: the allocation + header
// BRANCHES: kind switch + TLAB refill
// CACHE: fresh TLAB lines (streaming stores)
uint64_t helper_alloc_slow(J1Context* ctx, uint32_t kind, uint64_t a,
                           uint64_t b) noexcept {
    auto* heap = static_cast<gc::Heap*>(ctx->heap);
    tlab_adopt(ctx);
    void* raw = nullptr;
    switch (kind) {
    case kKindObject: {
        auto* klass = static_cast<Klass**>(ctx->klass_table)[
            static_cast<uint32_t>(a)];
        if (klass != nullptr) {
            auto obj = heap->allocate_object(
                klass, static_cast<uint32_t>(b));
            if (obj) raw = *obj;
        }
        break;
    }
    case kKindArray: {
        auto arr = heap->allocate_array(static_cast<uint32_t>(a));
        if (arr) raw = *arr;
        break;
    }
    case kKindDouble: {
        // `a` carries the full 64-bit payload (the template's movq rdx, xmm0
        // writes the whole register — the 32-bit form truncated doubles whose
        // payload has high bits, e.g. 5.0, into 0.0).
        double d = 0.0;
        std::memcpy(&d, &a, sizeof(d));
        auto boxed = heap->allocate_double(d);
        if (boxed) raw = *boxed;
        break;
    }
    default:
        break;
    }
    tlab_publish(ctx);
    return ptr_bits(raw);
}

// T0's generic arithmetic kernel (interpreter.cpp generic_add/sub/mul/
// compare semantics verbatim): overflow and type errors are distinct
// failures, both surfaced as kUndefinedRawBits so the generated error exit
// carries the stencil's own T0-worded error id.
bool both_smi(uint64_t a, uint64_t b) noexcept {
    return (a & 0x1) == 0 && (b & 0x1) == 0;
}

// @cold — canonical-arithmetic fallback behind every speculative stencil +
// the always-helper ops (division, saturating F64->I64). Implements
// exactly T0's semantics (docs/guest-semantics.md registered oracle).
uint64_t helper_generic_binop(J1Context* ctx, uint32_t op, uint64_t a,
                              uint64_t b) noexcept {
    const auto opid = static_cast<ugb::Op>(op);
    if (opid == ugb::Op::F64_TO_I64) {
        if ((a & 0xF) != 0x1) return kUndefinedRawBits;
        auto* obj = reinterpret_cast<HeapObject*>(a & ~uint64_t{0xF});
        auto* heap = static_cast<gc::Heap*>(ctx->heap);
        if (obj->header.klass != heap->double_klass()) {
            return kUndefinedRawBits;
        }
        const double d = read_boxed_double(obj);
        // T0's saturating conversion (L_F64_TO_I64): NaN -> 0, clamp.
        int64_t r = 0;
        if (d != d) {
            r = 0;
        } else if (d >= static_cast<double>(TaggedValue::smi_max())) {
            r = TaggedValue::smi_max();
        } else if (d <= static_cast<double>(TaggedValue::smi_min())) {
            r = TaggedValue::smi_min();
        } else {
            r = static_cast<int64_t>(d);
        }
        return static_cast<uint64_t>(TaggedValue::smi(r).raw());
    }
    if (opid == ugb::Op::DIV_S_I64 || opid == ugb::Op::REM_S_I64) {
        if (!both_smi(a, b)) return kUndefinedRawBits;
        const int64_t x = static_cast<int64_t>(a) >> 1;
        const int64_t y = static_cast<int64_t>(b) >> 1;
        if (y == 0) return kUndefinedRawBits;  // T0: division by zero
        if (opid == ugb::Op::DIV_S_I64 && x == TaggedValue::smi_min() &&
            y == -1) {
            return kUndefinedRawBits;  // T0: integer overflow
        }
        const int64_t r = opid == ugb::Op::DIV_S_I64 ? x / y : x % y;
        return static_cast<uint64_t>(TaggedValue::smi(r).raw());
    }
    switch (opid) {
    case ugb::Op::ADD_ANY:
    case ugb::Op::SUB_ANY:
    case ugb::Op::MUL_ANY: {
        if (!both_smi(a, b)) return kUndefinedRawBits;
        const int64_t x = static_cast<int64_t>(a) >> 1;
        const int64_t y = static_cast<int64_t>(b) >> 1;
        int64_t r = 0;
        bool ok = false;
        switch (opid) {
        case ugb::Op::ADD_ANY:
            ok = !__builtin_add_overflow(x, y, &r) &&
                 r >= TaggedValue::smi_min() && r <= TaggedValue::smi_max();
            break;
        case ugb::Op::SUB_ANY:
            ok = !__builtin_sub_overflow(x, y, &r) &&
                 r >= TaggedValue::smi_min() && r <= TaggedValue::smi_max();
            break;
        default:
            ok = !__builtin_mul_overflow(x, y, &r) &&
                 r >= TaggedValue::smi_min() && r <= TaggedValue::smi_max();
            break;
        }
        if (!ok) return kUndefinedRawBits;
        return static_cast<uint64_t>(TaggedValue::smi(r).raw());
    }
    case ugb::Op::EQ_I32:
    case ugb::Op::EQ_I64:
    case ugb::Op::NE_I64:
    case ugb::Op::LT_S_I64:
    case ugb::Op::LE_S_I64:
    case ugb::Op::GT_S_I64:
    case ugb::Op::GE_S_I64: {
        if (!both_smi(a, b)) return kUndefinedRawBits;
        const int64_t x = static_cast<int64_t>(a) >> 1;
        const int64_t y = static_cast<int64_t>(b) >> 1;
        bool r = false;
        switch (opid) {
        case ugb::Op::EQ_I32:
        case ugb::Op::EQ_I64: r = x == y; break;
        case ugb::Op::NE_I64: r = x != y; break;
        case ugb::Op::LT_S_I64: r = x < y; break;
        case ugb::Op::LE_S_I64: r = x <= y; break;
        case ugb::Op::GT_S_I64: r = x > y; break;
        default: r = x >= y; break;
        }
        return static_cast<uint64_t>(TaggedValue::boolean(r).raw());
    }
    default:
        return kUndefinedRawBits;
    }
}

// @cold — unresolved-field slow path: name-based resolution through the
// klass (the cold string compare the IC system exists to avoid; Rule 5).
uint64_t helper_get_field_slow(J1Context* ctx, uint64_t obj_bits,
                               uint32_t field_token) noexcept {
    const TaggedValue obj = TaggedValue::from_raw(obj_bits);
    if (!obj.is_heap_object()) return 0;
    auto* o = obj.as_heap_object();
    if (o->header.klass == nullptr) return 0;
    const auto* module = static_cast<const ugb::UGBModule*>(ctx->module);
    if (field_token >= module->fields.size()) return 0;
    const int idx =
        o->header.klass->find_field(module->fields[field_token].name);
    if (idx < 0) return 0;
    return static_cast<Object*>(o)->field(static_cast<uint32_t>(idx)).raw();
}

// @cold — unresolved-field store + ICGGC card marking. Returns 1 on
// success, 0 on failure (the template exits through kErrSetField).
uint32_t helper_set_field_slow(J1Context* ctx, uint64_t obj_bits,
                               uint64_t value_bits, uint32_t field_token) noexcept {
    const TaggedValue obj = TaggedValue::from_raw(obj_bits);
    const TaggedValue value = TaggedValue::from_raw(value_bits);
    if (!obj.is_heap_object()) return 0;
    auto* o = obj.as_heap_object();
    if (o->header.klass == nullptr) return 0;
    const auto* module = static_cast<const ugb::UGBModule*>(ctx->module);
    if (field_token >= module->fields.size()) return 0;
    const int idx =
        o->header.klass->find_field(module->fields[field_token].name);
    if (idx < 0) return 0;
    auto* objp = static_cast<Object*>(o);
    objp->field(static_cast<uint32_t>(idx)) = value;
    if (value.is_heap_object()) {
        static_cast<gc::Heap*>(ctx->heap)->card_table().mark_dirty(objp);
    }
    return 1;
}

// @cold — token-driven call: runs the callee through the attached T0
// interpreter (the M1 call path; direct-call patch points arrive with
// LDPT/J4 wiring). T0 may allocate: adopt/publish the TLAB snapshot.
int64_t helper_invoke_token(J1Context* ctx, uint32_t token,
                            const TaggedValue* args, uint32_t argc,
                            TaggedValue* out) noexcept {
    auto* module = static_cast<ugb::UGBModule*>(const_cast<void*>(ctx->module));
    auto* interp = static_cast<vm::Interpreter*>(ctx->interpreter);
    if (module == nullptr || interp == nullptr || token == 0 ||
        token > module->method_table.size()) {
        ctx->last_error = static_cast<uint32_t>(J1ErrorId::kErrCallToken);
        return static_cast<int64_t>(J1ErrorId::kErrCallToken);
    }
    ugb::UGBMethod& callee = module->method_table[token - 1];
    tlab_adopt(ctx);
    auto run = interp->run(*module, callee.name,
                           std::span<const TaggedValue>(args, argc));
    tlab_publish(ctx);
    if (!run) {
        ctx->last_error = static_cast<uint32_t>(J1ErrorId::kErrCallToken);
        return static_cast<int64_t>(J1ErrorId::kErrCallToken);
    }
    *out = run->value;
    return 0;
}

int64_t helper_invoke_builtin(J1Context* ctx, uint32_t token,
                              const TaggedValue* args, uint32_t argc,
                              TaggedValue* out) noexcept {
    auto* module = static_cast<ugb::UGBModule*>(const_cast<void*>(ctx->module));
    auto* interp = static_cast<vm::Interpreter*>(ctx->interpreter);
    if (module == nullptr || interp == nullptr) {
        ctx->last_error = static_cast<uint32_t>(J1ErrorId::kErrCallBuiltin);
        return static_cast<int64_t>(J1ErrorId::kErrCallBuiltin);
    }
    tlab_adopt(ctx);
    auto r = interp->invoke_builtin_token(
        *module, token, std::span<const TaggedValue>(args, argc));
    tlab_publish(ctx);
    if (!r) {
        ctx->last_error = static_cast<uint32_t>(J1ErrorId::kErrCallBuiltin);
        return static_cast<int64_t>(J1ErrorId::kErrCallBuiltin);
    }
    *out = *r;
    return 0;
}

// @cold — deopt materialization (M1 contract): record the failing pc; the
// driver reruns the method in T0 with the original arguments (RBPD region
// continuations land in M3).
int64_t helper_deopt(J1Context* ctx, uint32_t pc, const TaggedValue* vregs,
                     uint32_t count) noexcept {
    ctx->last_deopt_pc = pc;
    (void)vregs;
    (void)count;
    return static_cast<int64_t>(J1ErrorId::kErrDeopt);
}

uint32_t helper_throw(J1Context* ctx, uint32_t error_id) noexcept {
    ctx->last_error = error_id;
    return error_id;
}

uint64_t helper_write_barrier(J1Context* ctx, void* owner) noexcept {
    static_cast<gc::Heap*>(ctx->heap)->card_table().mark_dirty(owner);
    return 0;
}

// ---- patching --------------------------------------------------------------------

void push_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) {
        v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
    }
}

void patch_disp32_at(::vortex::codegen::CodeBuffer& buf, size_t at,
                          int32_t disp) noexcept {
    std::memcpy(buf.code().data() + at, &disp, 4);
}

void patch_disp32(uint8_t* at, int32_t disp) noexcept { std::memcpy(at, &disp, 4); }
void patch_imm64(uint8_t* at, uint64_t v) noexcept { std::memcpy(at, &v, 8); }
void patch_imm32(uint8_t* at, int32_t v) noexcept { std::memcpy(at, &v, 4); }

// vreg index for a VirtualRegister operand selector (corpus convention:
// 0 = dst, 1..3 = src0..src2, 4 = arg-window base = src0, 5 = frame base).
int32_t vreg_for_operand(const ugb::Instruction& ins, uint8_t operand) {
    switch (operand) {
    case 0: return static_cast<int32_t>(ins.dst);
    case 1:
    case 4: return ins.srcs.size() > 0 ? static_cast<int32_t>(ins.srcs[0]) : 0;
    case 2: return ins.srcs.size() > 1 ? static_cast<int32_t>(ins.srcs[1]) : 0;
    case 3: return ins.srcs.size() > 2 ? static_cast<int32_t>(ins.srcs[2]) : 0;
    default: return 0;  // 5 = whole-frame base (vreg 0)
    }
}

// Shared prologue/epilogue/OSR-copy emission (used by the entry and by the
// per-loop OSR stubs). Returns the offsets the caller must patch.
struct FramePreamble {
    size_t sub_site;      // imm32 of `sub rsp, frame`
    size_t vreg0_site;    // disp32 of `lea r9, [rbp + vreg0]`
    size_t copy_done;     // placeholder jcc to after the copy
    size_t copy_jcc;      // loop jcc (bound by the assembler)
    size_t end;           // first offset after the preamble
};

// copy source: rsi (walked), count: rcx, dst base: r9 (lea site).
FramePreamble emit_frame_preamble(x64::Assembler& a,
                                  ::vortex::codegen::CodeBuffer& buf,
                                  bool copy_args) {
    FramePreamble p{};
    a.push_reg(Reg::RBP);
    a.mov_reg_reg(Reg::RBP, Reg::RSP);
    a.push_reg(Reg::R15);
    a.push_reg(Reg::R14);
    p.sub_site = a.current_offset() + 3;  // REX + 81 + modrm, then imm32
    a.sub_reg_imm32(Reg::RSP, 0);
    a.mov_reg_reg(Reg::R15, Reg::RDI);  // ctx
    a.mov_reg_reg(Reg::R14, Reg::RCX);  // ret
    if (copy_args) {
        a.mov_reg_reg(Reg::RCX, Reg::RDX);  // argc
        a.xor_reg_reg(Reg::R9, Reg::R9);
        p.vreg0_site = a.current_offset() + 3;
        // Emit with a disp32 dummy so the patch site's 4-byte write matches
        // the encoding (a disp0 lea would encode disp8 and the patch would
        // clobber the following instruction).
        a.lea_reg_mem(Reg::R9,
                      Mem{Reg::RBP, Reg::RSP, 0, kTemplateVregDisp});
        a.test_reg_imm32(Reg::RCX, -1);
        p.copy_done = a.placeholder_jcc(x64::CC_E);
        const size_t copy_top = a.current_offset();
        a.mov_reg_mem(Reg::RAX, Mem{Reg::RSI, Reg::RSP, 0, 0});
        a.mov_mem_reg(Mem{Reg::R9, Reg::RSP, 0, 0}, Reg::RAX);
        a.add_reg_imm32(Reg::RSI, 8);
        a.add_reg_imm32(Reg::R9, 8);
        a.dec_reg(Reg::RCX);
        p.copy_jcc = a.placeholder_jcc(x64::CC_NE);
        // Backward edge: the loop header is BEFORE the body — the
        // assembler's bind_placeholder only binds forward, so patch the
        // rel32 directly to the loop top.
        patch_disp32_at(buf, p.copy_jcc,
                        static_cast<int32_t>(copy_top - (p.copy_jcc + 4)));
        a.bind_placeholder(p.copy_done);
    }
    p.end = a.current_offset();
    return p;
}

}  // namespace

// ---- context construction ---------------------------------------------------------

// @warm — once per engine binding. PERF_PERMIT PERF-006 (allocation on the
// compile/bind path, CEM-26 section 7):
// REASON: context setup is per-module-load @warm work; steady-state guest
//         execution allocates only through the corpus's inline TLAB paths.
// COST: O(constants + classes) allocations at bind time.
// OWNER: @vortex/rt (registered in docs/cem26.md section 5)
Result<void> make_j1_bindings(gc::Heap& heap, ugb::UGBModule& module,
                              vm::Interpreter* interpreter,
                              J1Bindings& bindings) {
    bindings.heap = &heap;

    // Klass table: reuse the T0-prepared table when present; otherwise
    // build a minimal registry from the module's class/field tokens (field
    // insertion order == field token order per klass).
    if (!module.runtime.klass_table.empty()) {
        for (void* k : module.runtime.klass_table) {
            bindings.klass_addr_table.push_back(k);
        }
    } else {
        bindings.owned_klasses.reserve(module.classes.size());
        for (const auto& c : module.classes) {
            Klass* k = bindings.registry.create(c.name);
            bindings.owned_klasses.push_back(k);
            bindings.klass_addr_table.push_back(k);
        }
        for (const auto& f : module.fields) {
            if (f.klass_token < bindings.owned_klasses.size()) {
                bindings.owned_klasses[f.klass_token]->add_field(f.name);
            }
        }
    }

    // Field token -> byte offset (16 + 8*slot); unresolved = -1.
    bindings.field_offsets.resize(module.fields.size(), -1);
    const bool t0_slots = !module.runtime.field_slot.empty();
    for (size_t tok = 0; tok < module.fields.size(); ++tok) {
        int32_t slot = -1;
        if (t0_slots) {
            if (tok < module.runtime.field_slot.size()) {
                slot = module.runtime.field_slot[tok];
            }
        } else {
            const uint32_t kt = module.fields[tok].klass_token;
            if (kt < bindings.owned_klasses.size()) {
                const int idx = bindings.owned_klasses[kt]->find_field(
                    module.fields[tok].name);
                slot = idx;  // find_field returns the slot index
            }
        }
        if (slot >= 0) {
            bindings.field_offsets[tok] =
                static_cast<int32_t>(sizeof(ObjectHeader)) + 8 * slot;
        }
    }

    // Constant pool materialization (Int64 -> Smi; Float64 -> boxed).
    bindings.constants.reserve(module.constants.size());
    for (const auto& c : module.constants) {
        switch (c.kind) {
        case ugb::Constant::Kind::Int64:
            if (c.i64 >= TaggedValue::smi_min() &&
                c.i64 <= TaggedValue::smi_max()) {
                bindings.constants.push_back(TaggedValue::smi(c.i64));
            } else {
                bindings.constants.push_back(TaggedValue::undefined());
            }
            break;
        case ugb::Constant::Kind::Float64: {
            auto boxed = heap.allocate_double(c.f64);
            bindings.constants.push_back(
                boxed ? TaggedValue::heap_pointer(*boxed)
                      : TaggedValue::undefined());
            break;
        }
        default:
            bindings.constants.push_back(TaggedValue::undefined());
            break;
        }
    }

    J1Context ctx{};
    ctx.heap = &heap;
    ctx.tlab_top = heap.tlab_top();
    ctx.tlab_end = heap.tlab_end();
    ctx.card_base = heap.card_table().data();
    ctx.heap_base = const_cast<void*>(heap.heap_base());
    ctx.constants = bindings.constants.data();
    ctx.klass_table = bindings.klass_addr_table.data();
    ctx.interpreter = interpreter;
    ctx.safepoint_word = &bindings.safepoint_word;
    ctx.alloc_slow = reinterpret_cast<void*>(&helper_alloc_slow);
    ctx.write_barrier = reinterpret_cast<void*>(&helper_write_barrier);
    ctx.deopt = reinterpret_cast<void*>(&helper_deopt);
    ctx.invoke_method = reinterpret_cast<void*>(&helper_invoke_token);
    ctx.invoke_builtin = reinterpret_cast<void*>(&helper_invoke_builtin);
    ctx.get_field_slow = reinterpret_cast<void*>(&helper_get_field_slow);
    ctx.set_field_slow = reinterpret_cast<void*>(&helper_set_field_slow);
    ctx.throw_error = reinterpret_cast<void*>(&helper_throw);
    ctx.generic_binop = reinterpret_cast<void*>(&helper_generic_binop);
    ctx.module = &module;
    // In-place: the context's pointers reference THIS bindings object's
    // vectors; `out` is caller-owned and never moved from here on.
    bindings.context = ctx;
    bindings.double_klass = heap.double_klass();
    return support::ok();
}

// ---- compile ------------------------------------------------------------------------

Result<BaselineCode> BaselineJit::compile(const BaselineJob& job) {
    if (job.module == nullptr ||
        job.method_id >= job.module->method_table.size()) {
        return fail(support::ErrorCode::InvalidArgument,
                    "J1: bad compile job");
    }
    ugb::UGBMethod& method =
        const_cast<ugb::UGBModule*>(job.module)->method_table[job.method_id];
    const uint32_t rc = method.register_count;
    const int32_t frame = frame_size(rc);

    // ---- pass 1: decode + select + native layout ---------------------------------
    ugb::InstructionStream stream(method.code.data(), method.code.size());
    struct Decoded {
        size_t pc;
        ugb::Instruction ins;
        const Stencil* stencil;
        size_t native_size;
    };
    std::vector<Decoded> seq;
    std::unordered_map<size_t, size_t> native_of;  // pc -> native offset

    {
        size_t pc = 0;
        size_t native = 0;
        while (pc < method.code.size()) {
            Decoded d;
            d.pc = pc;
            if (!stream.decode_at(pc, d.ins)) {
                return fail(support::ErrorCode::DecodeError,
                            "J1: malformed instruction stream");
            }
            // Generic variant first (profile-free M1); typed-only opcodes
            // fall back to their speculative stencil, whose in-template
            // canonical fallback keeps every input safe.
            const uint16_t op_id = static_cast<uint16_t>(d.ins.opcode);
            d.stencil = stencils_.select(op_id, false);
            if (d.stencil == nullptr) d.stencil = stencils_.select(op_id, true);
            if (d.stencil == nullptr) {
                return fail(
                    support::ErrorCode::Unimplemented,
                    "J1: no stencil for opcode " +
                        std::to_string(static_cast<int>(d.ins.opcode)) +
                        " (safe reject -> T0 fallback, Rule 3)");
            }
            d.native_size = d.stencil->bytes.size();
            native_of[d.pc] = native;
            native += d.native_size;
            seq.push_back(std::move(d));
        }
    }

    // ---- emit: entry preamble + body reservation + epilogue ---------------------
    ::vortex::codegen::CodeBuffer buf;
    x64::Assembler a(buf);
    const FramePreamble pro = emit_frame_preamble(a, buf, /*copy_args=*/true);
    const size_t body_off = buf.size();
    size_t native_end = body_off;
    for (const Decoded& d : seq) native_end += d.native_size;
    // Pass-1 offsets are body-relative; the preamble already occupies
    // [0, body_off) — shift them into absolute buffer coordinates before
    // instantiation (otherwise template 0 overwrites the prologue).
    for (auto& kv : native_of) kv.second += body_off;
    buf.code().resize(native_end, 0x00);

    // ---- pass 2: instantiate ------------------------------------------------------
    for (size_t i = 0; i < seq.size(); ++i) {
        const Decoded& d = seq[i];
        const Stencil& st = *d.stencil;
        const size_t inst_base = native_of[d.pc];
        std::memcpy(buf.code().data() + inst_base, st.bytes.data(),
                    st.bytes.size());
        const size_t next_native =
            i + 1 < seq.size() ? native_of[seq[i + 1].pc] : native_end;

        for (const PatchSite& ps : st.patch_sites) {
            uint8_t* at = buf.code().data() + inst_base + ps.offset;
            switch (ps.kind) {
            case PatchKind::VirtualRegister: {
                const int32_t v = vreg_for_operand(d.ins, ps.operand);
                patch_disp32(at, vreg_disp(rc, static_cast<uint32_t>(v)));
                break;
            }
            case PatchKind::ConstantIndex: {
                switch (ps.operand) {
                case 0: {  // PA_ConstSmi: (int64)(int32)meta << 1
                    const int64_t v =
                        static_cast<int64_t>(static_cast<int32_t>(d.ins.meta));
                    patch_imm64(at, static_cast<uint64_t>(v) << 1);
                    break;
                }
                case 1:  // PA_ConstPoolOff: 8 * constant index
                    patch_imm32(at, static_cast<int32_t>(8u * d.ins.meta));
                    break;
                case 3: {  // PA_KlassAddr
                    // F64-family templates guard against the heap's boxed-
                    // double klass; object templates against klass_table.
                    uint64_t addr = 0;  // 0 = never-matching guard sentinel
                    switch (d.ins.opcode) {
                    case ugb::Op::ADD_F64:
                    case ugb::Op::SUB_F64:
                    case ugb::Op::MUL_F64:
                    case ugb::Op::DIV_F64:
                    case ugb::Op::NEG_F64:
                    case ugb::Op::I64_TO_F64:
                    case ugb::Op::F64_TO_I64:
                    case ugb::Op::EQ_F64:
                    case ugb::Op::LT_F64:
                    case ugb::Op::LE_F64:
                    case ugb::Op::GT_F64:
                    case ugb::Op::GE_F64:
                        addr = ptr_bits(job.double_klass);
                        break;
                    case ugb::Op::NEW_OBJECT:
                    case ugb::Op::CHECK_CLASS:
                        if (job.klass_addrs != nullptr &&
                            d.ins.meta < job.klass_addrs->size() &&
                            (*job.klass_addrs)[d.ins.meta] != nullptr) {
                            addr = ptr_bits((*job.klass_addrs)[d.ins.meta]);
                        }
                        break;
                    default:
                        break;  // field guards: profile-driven (M2)
                    }
                    patch_imm64(at, addr);
                    break;
                }
                case 4: {  // PA_FieldByteOff: 16 + 8*slot
                    int32_t byte_off =
                        static_cast<int32_t>(sizeof(ObjectHeader));
                    if (job.field_offsets != nullptr &&
                        d.ins.meta < job.field_offset_count &&
                        job.field_offsets[d.ins.meta] >= 0) {
                        byte_off = job.field_offsets[d.ins.meta];
                    }
                    patch_imm32(at, byte_off);
                    break;
                }
                case 5: {  // PA_FieldCount
                    int32_t count = 0;
                    if (d.ins.opcode == ugb::Op::NEW_OBJECT &&
                        job.klass_addrs != nullptr &&
                        d.ins.meta < job.klass_addrs->size() &&
                        (*job.klass_addrs)[d.ins.meta] != nullptr) {
                        count = static_cast<int32_t>(
                            static_cast<Klass*>((*job.klass_addrs)[d.ins.meta])
                                ->field_count());
                    }
                    patch_imm32(at, count);
                    break;
                }
                case 8:  // PA_CallToken
                    patch_imm32(at, static_cast<int32_t>(d.ins.meta));
                    break;
                case 11:  // PA_MaxArrayLen
                    patch_imm32(at, static_cast<int32_t>(kJ1MaxArrayLength));
                    break;
                case 10:  // PA_KlassToken
                    patch_imm32(at, static_cast<int32_t>(d.ins.meta));
                    break;
                case 12:  // PA_FieldToken
                    patch_imm32(at, static_cast<int32_t>(d.ins.meta));
                    break;
                case 13:  // PA_CallArgc: the literal in the src1 field
                    patch_imm32(
                        at,
                        d.ins.srcs.size() > 1
                            ? static_cast<int32_t>(d.ins.srcs[1])
                            : 0);
                    break;
                default:
                    break;  // 2/6/7/9 unused by the M1 corpus
                }
                break;
            }
            case PatchKind::BranchTarget: {
                size_t target_native = native_end;
                if (ps.operand == 0) {
                    const auto it = native_of.find(d.ins.meta);
                    if (it == native_of.end()) {
                        return fail(support::ErrorCode::DecodeError,
                                    "J1: branch to instruction boundary");
                    }
                    target_native = it->second;
                } else if (ps.operand == 3) {
                    target_native = next_native;  // falls into the epilogue
                }
                // operands 1/2: both epilogue operands share the one body.
                const int32_t rel = static_cast<int32_t>(
                    static_cast<int64_t>(target_native) -
                    static_cast<int64_t>(inst_base + ps.offset + 4));
                patch_imm32(at, rel);
                break;
            }
            case PatchKind::IcSlot: {
                if (ps.operand == 1) {
                    // rel32 to this instance's slow body (always-slow jmp).
                    const size_t slow_native = inst_base + st.slow_path_offset;
                    const int32_t rel = static_cast<int32_t>(
                        static_cast<int64_t>(slow_native) -
                        static_cast<int64_t>(inst_base + ps.offset + 4));
                    patch_imm32(at, rel);
                }
                // operand 0 (guard imm) stays at the 0 sentinel until IC
                // profile plumbing lands (M2): the always-slow path is
                // correct for every receiver.
                break;
            }
            default:
                break;  // CardTableBase/ThreadLocalSlot/ProfileCounter/
                        // BytecodePc/DeoptHandle land with M2 plumbing
            }
        }
    }

    // ---- epilogue: pop r14; pop r15; add rsp, frame; pop rbp; ret ------------
    // rax carries 0 (normal; *ret written) or the error id.
    // Deallocate the local frame FIRST, then restore the saved registers in
    // reverse push order (prologue: push rbp, r15, r14). Popping before the
    // add would read the frame's bottom — stale memory — into r14/r15 and
    // silently corrupt every caller-side callee-saved register.
    const size_t epi_sub = a.current_offset() + 3;  // REX + 81 + modrm
    a.add_reg_imm32(Reg::RSP, 0);
    a.pop_reg(Reg::R14);
    a.pop_reg(Reg::R15);
    a.pop_reg(Reg::RBP);
    a.ret();
    patch_imm32(buf.code().data() + epi_sub, frame);
    patch_imm32(buf.code().data() + pro.sub_site, frame);
    patch_disp32(buf.code().data() + pro.vreg0_site, vreg_disp(rc, 0));

    // ---- OSR entry stubs (one per backward-branch target) ----------------------
    // J1OsrFn(ctx=rdi, vreg_state=rsi, count=edx, ret=rcx): fresh frame, all
    // vregs materialized from the snapshot, jump to the loop head.
    BaselineCode out;
    out.method_id = job.method_id;
    out.osr_entry_offset = kNoOsr;
    std::vector<std::pair<size_t, uint32_t>> osr_fixups;  // (rel32 off, pc)
    std::unordered_map<uint32_t, size_t> osr_stub_of;     // pc -> stub offset
    for (const Decoded& d : seq) {
        if (!ugb::is_branch(d.ins.opcode)) continue;
        if (d.ins.meta >= d.pc) continue;  // backward only
        if (osr_stub_of.count(static_cast<uint32_t>(d.ins.meta))) continue;
        osr_stub_of[static_cast<uint32_t>(d.ins.meta)] = buf.size();
        out.osr_entries.push_back(static_cast<uint32_t>(d.ins.meta));
        if (out.osr_entry_offset == kNoOsr) {
            out.osr_entry_offset = static_cast<uint32_t>(buf.size());
        }
        const FramePreamble pre = emit_frame_preamble(a, buf, /*copy_args=*/false);
        // Snapshot copy: rcx = count (edx raw), rsi walks the state array.
        a.mov_reg_reg(Reg::RCX, Reg::RDX);
        a.xor_reg_reg(Reg::R9, Reg::R9);
        const size_t v0 = a.current_offset() + 3;
        a.lea_reg_mem(Reg::R9,
                      Mem{Reg::RBP, Reg::RSP, 0, kTemplateVregDisp});
        a.test_reg_imm32(Reg::RCX, -1);
        const size_t done = a.placeholder_jcc(x64::CC_E);
        const size_t snap_top = a.current_offset();
        a.mov_reg_mem(Reg::RAX, Mem{Reg::RSI, Reg::RSP, 0, 0});
        a.mov_mem_reg(Mem{Reg::R9, Reg::RSP, 0, 0}, Reg::RAX);
        a.add_reg_imm32(Reg::RSI, 8);
        a.add_reg_imm32(Reg::R9, 8);
        a.dec_reg(Reg::RCX);
        const size_t loop = a.placeholder_jcc(x64::CC_NE);
        patch_disp32_at(buf, loop,
                        static_cast<int32_t>(snap_top - (loop + 4)));
        a.bind_placeholder(done);
        const size_t jmp_site = a.current_offset() + 1;
        a.jmp_rel32(0);
        osr_fixups.emplace_back(jmp_site, static_cast<uint32_t>(d.ins.meta));
        patch_imm32(buf.code().data() + pre.sub_site, frame);
        patch_disp32(buf.code().data() + v0, vreg_disp(rc, 0));
    }
    for (const auto& [site, target_pc] : osr_fixups) {
        const auto it = native_of.find(target_pc);
        if (it == native_of.end()) continue;
        const int32_t rel =
            static_cast<int32_t>(static_cast<int64_t>(it->second) -
                                 static_cast<int64_t>(site + 4));
        patch_imm32(buf.code().data() + site, rel);
    }

    // ---- metadata: GC maps (per call site, conservative all-vregs) -------------
    {
        const uint32_t map_bytes = (rc + 7) / 8;
        std::vector<uint8_t> ones(map_bytes, 0xFF);
        uint32_t count = 0;
        for (const Decoded& d : seq) {
            if (!ugb::is_call(d.ins.opcode)) continue;
            ++count;
        }
        push_u32(out.gc_maps, count);
        for (const Decoded& d : seq) {
            if (!ugb::is_call(d.ins.opcode)) continue;
            push_u32(out.gc_maps, static_cast<uint32_t>(d.pc));
            push_u32(out.gc_maps, map_bytes);
            out.gc_maps.insert(out.gc_maps.end(), ones.begin(), ones.end());
        }
    }

    // ---- metadata: compact deopt records (docs/tier-j1.md section 8) -----------
    {
        uint32_t count = 0;
        for (const Decoded& d : seq) {
            if (d.stencil->speculative || ugb::is_call(d.ins.opcode) ||
                d.ins.opcode == ugb::Op::SAFEPOINT_POLL) {
                ++count;
            }
        }
        push_u32(out.deopt_records, count);
        for (const Decoded& d : seq) {
            if (!(d.stencil->speculative || ugb::is_call(d.ins.opcode) ||
                  d.ins.opcode == ugb::Op::SAFEPOINT_POLL)) {
                continue;
            }
            push_u32(out.deopt_records, static_cast<uint32_t>(d.pc));
            push_u32(out.deopt_records, rc);       // frame descriptor id
            push_u32(out.deopt_records, 0);        // register map: full frame
            push_u32(out.deopt_records, 0);        // ic state snapshot
        }
    }

    out.code = std::move(buf.code());
    return out;
}

// ---- publish + run ------------------------------------------------------------------

Result<BaselineExecutable> publish_baseline(const BaselineCode& code) {
    auto mem = infra::WritableCodeMemory::allocate(code.code.size());
    if (!mem) return std::unexpected(std::move(mem).error());
    mem->write(code.code, 0);
    if (auto r = mem->publish(); !r) {
        return std::unexpected(std::move(r).error());
    }
    BaselineExecutable ex;
    ex.memory = std::move(*mem);
    ex.entry = reinterpret_cast<J1EntryFn>(ex.memory.data());
    if (code.osr_entry_offset != kNoOsr &&
        code.osr_entry_offset < code.code.size()) {
        ex.osr_entry = reinterpret_cast<J1OsrFn>(ex.memory.data() +
                                                 code.osr_entry_offset);
    }
    ex.method_id = code.method_id;
    return ex;
}

Result<TaggedValue> run_baseline(BaselineExecutable& ex, J1Bindings& bindings,
                                 std::span<const TaggedValue> args) {
    TaggedValue ret;
    // Adopt the heap's current TLAB view (intervening T0 allocations may
    // have bumped it since the binding was made), then publish back after.
    tlab_adopt(&bindings.context);
    const int64_t rc =
        ex.entry(&bindings.context, args.data(),
                 static_cast<uint32_t>(args.size()), &ret);
    tlab_publish(&bindings.context);  // J1 bumped the snapshot natively
    if (rc == static_cast<int64_t>(J1ErrorId::kErrDeopt)) {
        // M1 suspension contract (corpus SAFEPOINT_POLL): rerun the whole
        // method in the attached T0 with the original arguments. RBPD
        // region continuations replace this in M3.
        auto* module =
            static_cast<ugb::UGBModule*>(const_cast<void*>(bindings.context.module));
        auto* interp = static_cast<vm::Interpreter*>(bindings.context.interpreter);
        if (interp != nullptr && module != nullptr &&
            ex.method_id < module->method_table.size()) {
            tlab_adopt(&bindings.context);
            auto run = interp->run(
                *module, module->method_table[ex.method_id].name, args);
            tlab_publish(&bindings.context);
            if (run) return run->value;
        }
    }
    if (rc != 0) {
        return fail(support::ErrorCode::RuntimeError,
                    std::string("J1 runtime error: ") +
                        diagnose_j1_error(bindings.context.last_error));
    }
    return ret;
}

const char* diagnose_j1_error(uint32_t id) noexcept {
    switch (static_cast<J1ErrorId>(id)) {
    case J1ErrorId::kErrDeopt: return "safepoint request (deopt to T0)";
    case J1ErrorId::kErrAddUnsupported:
        return "Add.Any: unsupported operand types";
    case J1ErrorId::kErrAddTyped: return "Add.I32: speculation failed";
    case J1ErrorId::kErrAddI64: return "Add.I64: overflow or non-integer operand";
    case J1ErrorId::kErrAddF64: return "Add.F64: operand is not a float";
    case J1ErrorId::kErrSubAny: return "Sub.Any: unsupported operand types";
    case J1ErrorId::kErrSubTyped: return "Sub.I32: speculation failed";
    case J1ErrorId::kErrSubF64: return "Sub.F64: operand is not a float";
    case J1ErrorId::kErrMulAny: return "Mul.Any: unsupported operand types";
    case J1ErrorId::kErrMulTyped: return "Mul.I32: speculation failed";
    case J1ErrorId::kErrMulF64: return "Mul.F64: operand is not a float";
    case J1ErrorId::kErrDivS: return "Div.S.I64: integer overflow";
    case J1ErrorId::kErrDivF64: return "Div.F64: operand is not a float";
    case J1ErrorId::kErrDivZero: return "division by zero";
    case J1ErrorId::kErrRemS: return "Rem.S.I64: non-integer operand";
    case J1ErrorId::kErrRemZero: return "remainder by zero";
    case J1ErrorId::kErrNegI: return "Neg.I64: non-integer or overflow";
    case J1ErrorId::kErrNegF64: return "Neg.F64: operand is not a float";
    case J1ErrorId::kErrCompare:
        return "comparison: unsupported operand types";
    case J1ErrorId::kErrEqF64: return "Eq.F64: operand is not a float";
    case J1ErrorId::kErrCmpF64: return "float compare: not a float";
    case J1ErrorId::kErrI64ToF64: return "I64ToF64: non-integer operand";
    case J1ErrorId::kErrF64ToI64: return "F64ToI64: operand is not a float";
    case J1ErrorId::kErrCallToken: return "Call: unresolved callee";
    case J1ErrorId::kErrCallBuiltin:
        return "Call.Builtin: unresolved builtin token";
    case J1ErrorId::kErrNewObject: return "New.Object: bad klass token";
    case J1ErrorId::kErrNewArrayLen: return "New.Array: bad length";
    case J1ErrorId::kErrAllocOOM: return "allocation failed";
    case J1ErrorId::kErrGetField: return "GetField: unresolved field";
    case J1ErrorId::kErrSetField:
        return "SetField: unresolved field or bad receiver";
    case J1ErrorId::kErrArrayLen: return "Array.Length: not an array";
    case J1ErrorId::kErrArrayGet: return "Array.Get: bad array or index";
    case J1ErrorId::kErrArraySet: return "Array.Set: bad array or index";
    case J1ErrorId::kErrBounds: return "bounds check failed";
    case J1ErrorId::kErrCheckNull: return "Check.Null: value is not null";
    case J1ErrorId::kErrCheckNonNull: return "Check.NonNull: null dereference";
    case J1ErrorId::kErrCheckClass: return "Check.Class: class guard failed";
    case J1ErrorId::kErrUnreachable: return "unreachable executed";
    case J1ErrorId::kErrNoStencil: return "no stencil for opcode";
    default: return "unknown J1 error";
    }
}

}  // namespace vortex::j1
