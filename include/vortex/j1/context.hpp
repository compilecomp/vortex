// J1 native execution ABI (docs/tier-j1.md sections 4 and 8).
//
// The stencil corpus generates x86-64 machine code against the contracts in
// this header: a J1-compiled method is a SysV function
//
//     int64_t entry(J1Context* rdi, const TaggedValue* rsi args,
//                   uint32_t edx argc, TaggedValue* rcx ret)
//
// returning 0 on normal completion (result stored through `ret`) or a
// non-zero J1ErrorId on a runtime error (matching the observable "runtime
// error" behavior of T0). Deopt requests exit through the same channel with
// J1ErrorId::Deopt and leave the failing pc in J1Context::last_deopt_pc.
//
// Register conventions inside generated method bodies:
//     r15 = ctx (callee-saved), r14 = ret pointer (callee-saved),
//     vreg i lives at [rbp + vreg_disp(i)] (ascending, so an argument
//     window v[s0 .. s0+argc) is a contiguous TaggedValue array).
//
// Helper functions are reached as `call qword [r15 + field]` — an indirect
// call through the context, so no code-buffer relocations are needed for
// runtime entry points. Field offsets are pinned by static_asserts: data
// fields sit in the disp8 window (< 0x80), function-pointer fields at
// >= 0x80 so every helper call has one uniform encoding.
#pragma once

#include <cstddef>
#include <cstdint>

#include "vortex/support/tagged_value.hpp"

namespace vortex::j1 {

struct J1Context;

/// Normal entry of a J1-compiled method.
using J1EntryFn = int64_t (*)(J1Context* ctx, const TaggedValue* args,
                              uint32_t argc, TaggedValue* ret);

/// OSR entry (docs/tier-j1.md section 8): fills all vregs from `vreg_state`
/// (a T0 register file snapshot, count == method register_count) and jumps
/// straight to the loop header compiled at a fixed bytecode pc.
using J1OsrFn = int64_t (*)(J1Context* ctx, const TaggedValue* vreg_state,
                            uint32_t count, TaggedValue* ret);

// ---- helper prototypes (implemented in src/j1/baseline_jit.cpp) -------------

/// Slow allocation path (TLAB miss). kind: 0 = object(klass_token = a,
/// field_count = b), 1 = array(length = a), 2 = boxed double (payload bits
/// in a — 64-bit, the template's movq writes the whole register). Returns
/// the raw (untagged) pointer or 0 on OOM.
using J1AllocSlowFn = uint64_t (*)(J1Context*, uint32_t kind, uint64_t a,
                                   uint64_t b);

/// Card-table write barrier: marks the card owning `owner` dirty.
using J1WriteBarrierFn = void (*)(J1Context*, void* owner);

/// Deopt materialization: snapshots all `count` vregs plus the failing pc.
/// Writes the deopted result through the entry's ret pointer (kept in r14)
/// and returns 0; the generated thunk then takes the normal exit.
using J1DeoptFn = int64_t (*)(J1Context*, uint32_t pc,
                              const TaggedValue* vregs, uint32_t count);

/// Token-driven call (unresolved CALL_DIRECT / CALL_VIRTUAL fallback):
/// runs the callee through the attached T0 interpreter, mirroring T0
/// observable behavior. Returns 0 (value in *out) or an error id.
using J1InvokeTokenFn = int64_t (*)(J1Context*, uint32_t token,
                                    const TaggedValue* args, uint32_t argc,
                                    TaggedValue* out);

/// CALL_BUILTIN with lazy name resolution + per-token caching.
using J1GetFieldSlowFn = uint64_t (*)(J1Context*, uint64_t obj_bits,
                                      uint32_t field_token);
using J1SetFieldSlowFn = uint32_t (*)(J1Context*, uint64_t obj_bits,
                                      uint64_t value_bits,
                                      uint32_t field_token);
using J1ThrowFn = uint32_t (*)(J1Context*, uint32_t error_id);

/// Canonical-arithmetic fallback shared by typed stencil failure paths.
/// Implements exactly T0's generic_add/sub/mul/compare/bit semantics; a
/// return of kUndefinedRawBits means "T0 would raise a runtime error here".
using J1GenericBinopFn = uint64_t (*)(J1Context*, uint32_t op, uint64_t a,
                                      uint64_t b);

/// Runtime error ids (stencils load these as immediates; the runtime maps
/// them to diagnostics with the same wording T0 uses).
enum J1ErrorId : uint32_t {
    kErrNone = 0,
    kErrDeopt = 1,           // deopt handler ran; result written through ret
    kErrAddUnsupported = 2,  // "Add.Any: unsupported operand types"
    kErrAddTyped = 3,        // "Add.I32: speculation failed and canonical ..."
    kErrAddI64 = 4,          // "Add.I64: overflow or non-integer operand"
    kErrAddF64 = 5,
    kErrSubAny = 6,
    kErrSubTyped = 7,
    kErrSubF64 = 8,
    kErrMulAny = 9,
    kErrMulTyped = 10,
    kErrMulF64 = 11,
    kErrDivS = 12,
    kErrDivF64 = 13,
    kErrDivZero = 14,
    kErrRemS = 15,
    kErrRemZero = 16,
    kErrNegI = 17,
    kErrNegF64 = 18,
    kErrBitop = 19,
    kErrCompare = 20,
    kErrEqF64 = 21,
    kErrCmpF64 = 22,
    kErrI64ToF64 = 23,
    kErrF64ToI64 = 24,
    kErrCallToken = 25,
    kErrCallBuiltin = 26,
    kErrCallVirtual = 27,
    kErrNewObject = 28,
    kErrNewArrayLen = 29,
    kErrAllocOOM = 30,
    kErrGetField = 31,
    kErrSetField = 32,
    kErrArrayLen = 33,
    kErrArrayGet = 34,
    kErrArraySet = 35,
    kErrBounds = 36,
    kErrCheckNull = 37,
    kErrCheckNonNull = 38,
    kErrCheckClass = 39,
    kErrCheckBounds = 40,
    kErrUnreachable = 41,
    kErrNoStencil = 42,
};

/// The generated-code view of the runtime. Field offsets are ABI:
/// generated code hard-codes them (pinned below by static_asserts).
struct J1Context {
    // ---- hot data (disp8 window, offsets < 0x80) --------------------------
    void* heap;                    // 0x00  gc::Heap*
    void* tlab_top;                // 0x08  allocation bump pointer
    void* tlab_end;                // 0x10  TLAB end (refilled by alloc_slow)
    void* card_base;               // 0x18  CardTable::data()
    void* heap_base;               // 0x20  card-index origin
    const TaggedValue* constants;  // 0x28  materialized constant pool
    void* klass_table;             // 0x30  Klass* per class token
    void* interpreter;             // 0x38  vm::Interpreter* (T0 fallback)
    void* safepoint_word;          // 0x40  uint32_t* global poll flag
    uint8_t reserved_0x48[0x38];   // reserved for M2+ hot fields (IC slots)

    // ---- helper functions (>= 0x80: uniform disp32 indirect calls) --------
    void* alloc_slow;              // 0x80  J1AllocSlowFn
    void* write_barrier;           // 0x88  J1WriteBarrierFn
    void* deopt;                   // 0x90  J1DeoptFn
    void* invoke_method;           // 0x98  J1InvokeTokenFn
    void* invoke_builtin;          // 0xA0  J1InvokeTokenFn
    void* get_field_slow;          // 0xA8  J1GetFieldSlowFn
    void* set_field_slow;          // 0xB0  J1SetFieldSlowFn
    void* throw_error;             // 0xB8  J1ThrowFn
    void* generic_binop;           // 0xC0  J1GenericBinopFn
    const void* module;            // 0xC8  const ugb::UGBModule*
    uint64_t poll_count;           // 0xD0  safepoint polls executed
    uint32_t last_error;           // 0xD8  error id of the last exit
    uint32_t last_deopt_pc;        // 0xDC  failing bytecode pc (deopt/RBPD)
};

// ABI pins (the stencil corpus emits these offsets as immediates).
static_assert(offsetof(J1Context, tlab_top) == 0x08);
static_assert(offsetof(J1Context, tlab_end) == 0x10);
static_assert(offsetof(J1Context, card_base) == 0x18);
static_assert(offsetof(J1Context, heap_base) == 0x20);
static_assert(offsetof(J1Context, constants) == 0x28);
static_assert(offsetof(J1Context, klass_table) == 0x30);
static_assert(offsetof(J1Context, safepoint_word) == 0x40);
static_assert(offsetof(J1Context, alloc_slow) == 0x80);
static_assert(offsetof(J1Context, write_barrier) == 0x88);
static_assert(offsetof(J1Context, deopt) == 0x90);
static_assert(offsetof(J1Context, invoke_method) == 0x98);
static_assert(offsetof(J1Context, invoke_builtin) == 0xA0);
static_assert(offsetof(J1Context, get_field_slow) == 0xA8);
static_assert(offsetof(J1Context, set_field_slow) == 0xB0);
static_assert(offsetof(J1Context, throw_error) == 0xB8);
static_assert(offsetof(J1Context, generic_binop) == 0xC0);
static_assert(offsetof(J1Context, module) == 0xC8);
static_assert(offsetof(J1Context, poll_count) == 0xD0);
static_assert(offsetof(J1Context, last_error) == 0xD8);
static_assert(offsetof(J1Context, last_deopt_pc) == 0xDC);

/// Raw tagged bits of `undefined` — the canonical-binop helper returns this
/// to mean "T0 raises a runtime error here" (0x7 is not a legal Smi or
/// heap-pointer encoding).
constexpr uint64_t kUndefinedRawBits = 0x7;

/// Vreg frame addressing. The frame keeps every displacement outside the
/// disp8 window so all vreg patch sites share one uniform disp32 encoding:
///     vreg i @ [rbp + vreg_disp(rc, i)]  =  [rbp - frame_size(rc) + 8*i]
/// Prologue: push rbp; mov rbp,rsp; push r15; push r14; sub rsp, frame_size.
/// (Entry rsp ≡ 8 mod 16; three pushes land rsp ≡ 0; frame_size is a
/// multiple of 16 so call sites keep the ABI-aligned stack.)
constexpr int32_t kVregFramePad = 128;
inline int32_t frame_size(uint32_t register_count) noexcept {
    const int32_t bytes = static_cast<int32_t>(8u * register_count);
    return ((bytes + 15) & ~15) + kVregFramePad;
}
inline int32_t vreg_disp(uint32_t register_count, uint32_t vreg) noexcept {
    // Locals live BELOW rbp (push rbp; mov rbp,rsp; sub rsp, frame): the
    // displacement is negative, vreg 0 lowest. The magnitude stays outside
    // the disp8 window (frame >= 144) so every site keeps its disp32 form.
    return -frame_size(register_count) + static_cast<int32_t>(8u * vreg);
}

}  // namespace vortex::j1
