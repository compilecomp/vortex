// UGB opcode ISA (docs/ugb.md section 4). Opcodes are u16 per the standard's
// binary encoding. Canonical (generic) and speculative (typed) forms both live
// in this enum: T0 executes both and rewrites between them, exactly as
// docs/tier-t0.md section 2 describes.
#pragma once

#include <cstdint>
#include <string_view>

namespace vortex::ugb {

// Flags byte of an instruction word.
enum InstrFlags : uint8_t {
    FLAG_NONE = 0,
    FLAG_HAS_META = 0x1,  // metadata_token present in the stream
};

// Instruction operand-role conventions (documented per opcode group):
//   CONST_*      : dst, meta = constant pool index (I64/F64/String)
//                  CONST_I32 carries a signed 32-bit immediate in meta
//   binops       : dst, src0, src1
//   comparisons  : dst, src0, src1                (dst = boolean)
//   GET_FIELD*   : dst, obj                        meta = field token
//   SET_FIELD*   : obj, value                      meta = field token
//   CALL_*       : dst(result), arg_base           meta = callee token,
//                                                  src1 = argc
//   JUMP*        : meta = absolute bytecode offset of the target
//   CHECK_*      : value, extra                    meta = token/limit
//   NEW_*        : dst                             meta = token (klass)
//   ARRAY_*      : documented per opcode
enum class Op : uint16_t {
    // ---- 0. illegal / padding ---------------------------------------------
    ILLEGAL = 0,

    // ---- 1. constants and moves -------------------------------------------
    CONST_NULL,
    CONST_UNDEFINED,
    CONST_FALSE,
    CONST_TRUE,
    CONST_I32,     // meta = signed immediate
    CONST_I64,     // meta = constant pool index (int64)
    CONST_F64,     // meta = constant pool index (double)
    CONST_STRING,  // meta = constant pool index (string)
    CONST_METHOD,  // meta = method token
    MOVE,
    SWAP,
    COPY,

    // ---- 2. integer arithmetic (canonical + typed forms) -------------------
    ADD_ANY,
    ADD_I32,
    ADD_I64,
    ADD_F64,
    ADD_CHECKED_I32,
    ADD_CHECKED_I64,
    SUB_ANY,
    SUB_I32,
    SUB_I64,
    SUB_F64,
    SUB_CHECKED_I32,
    MUL_ANY,
    MUL_I32,
    MUL_I64,
    MUL_F64,
    MUL_CHECKED_I32,
    DIV_S_I64,
    DIV_U_I64,
    DIV_F64,
    REM_S_I64,
    REM_U_I64,
    NEG_I64,
    NEG_F64,

    // ---- 3. bit operations (I64 payload) -----------------------------------
    AND_I,
    OR_I,
    XOR_I,
    SHL_I,
    SHR_S_I,
    SHR_U_I,

    // ---- 4. comparisons ----------------------------------------------------
    EQ_I32,
    EQ_I64,
    EQ_F64,
    EQ_REF,
    EQ_NULL,
    NE_I64,
    NE_REF,
    LT_S_I64,
    LE_S_I64,
    GT_S_I64,
    GE_S_I64,
    LT_F64,
    LE_F64,
    GT_F64,
    GE_F64,
    EQ_ANY,       // canonical: language hook via site
    COMPARE_ANY,  // canonical: language-defined ordering

    // ---- 5. conversions ----------------------------------------------------
    I64_TO_F64,
    F64_TO_I64,
    SEXT_I32_I64,
    TRUNC_I64_I32,
    BOX,
    UNBOX,
    ANY_TO_TYPED,
    TYPED_TO_ANY,

    // ---- 6. control flow ---------------------------------------------------
    JUMP,
    JUMP_TRUE,
    JUMP_FALSE,
    JUMP_EQ,
    JUMP_NE,
    RETURN,
    RETURN_UNIT,
    UNREACHABLE,
    SWITCH_INT,

    // ---- 7. calls and dispatch ---------------------------------------------
    CALL_DIRECT,
    CALL_VIRTUAL,
    CALL_INTERFACE,
    CALL_DYNAMIC,
    CALL_CLOSURE,
    CALL_BUILTIN,
    CALL_FOREIGN,
    TAIL_CALL_DIRECT,

    // ---- 8. allocation ------------------------------------------------------
    NEW_OBJECT,   // meta = klass token
    NEW_ARRAY,    // src0 = length

    // ---- 9. field / property access -----------------------------------------
    GET_FIELD,         // meta = field token
    SET_FIELD,         // meta = field token
    GET_FIELD_SHAPE,   // meta = field token, shape-guarded (speculative)
    SET_FIELD_SHAPE,
    GET_PROP_DYNAMIC,  // meta = name token (hook fallback)
    SET_PROP_DYNAMIC,
    DELETE_PROP_DYNAMIC,
    HAS_PROP_DYNAMIC,

    // ---- 10. arrays ----------------------------------------------------------
    ARRAY_LENGTH,          // dst, array
    ARRAY_GET,             // dst, array, index      (bounds-checked)
    ARRAY_SET,             // array, index, value    (bounds-checked)
    ARRAY_GET_UNCHECKED,
    ARRAY_SET_UNCHECKED,

    // ---- 11. type checks and guards ------------------------------------------
    CHECK_NULL,
    CHECK_NON_NULL,
    CHECK_CLASS,   // meta = klass token (speculative)
    CHECK_BOUNDS,
    CHECK_SHAPE,

    // ---- 12. atomics / concurrency ---------------------------------------------
    ATOMIC_LOAD,
    ATOMIC_STORE,
    FENCE_SEQ_CST,
    SAFEPOINT_POLL,

    // ---- 13. closures -----------------------------------------------------------
    CLOSURE_NEW,
    CLOSURE_GET_UPVALUE,
    CLOSURE_SET_UPVALUE,

    // ---- 14. exceptions -----------------------------------------------------------
    TRY_BEGIN,
    TRY_END,
    THROW,

    // ---- 15. GC barrier hints ------------------------------------------------------
    WRITE_BARRIER_STORE,

    // ---- 15a. interop messages (docs/interop-protocol.md section 5) ---------------
    // Hot interop messages hold dedicated opcodes so the tiers can
    // devirtualize and inline them; the generic send is the fallback.
    // Loader law: the module's capability mask must carry the matching
    // CAP_INTEROP_* bit (runtime/interop.hpp) or the load is rejected.
    POLY_EXECUTE,  // dst(result), arg_base, argc; meta = language/member ctx
    POLY_READ,     // dst, recv, member_idx
    POLY_WRITE,    // recv, member_idx, value
    POLY_SEND,     // dst, msg_id, recv, arg_base (cold/generic fallback)

    // ---- 16. debug -------------------------------------------------------------------
    DEBUG_SRCPOS,  // meta = line number
    DEBUG_TRAP,

    // ---- 17. extensions ----------------------------------------------------------------
    EXT_OP,  // meta = extension opcode id registered in the module

    // sentinel
    _COUNT,
};

constexpr uint16_t kOpcodeCount = static_cast<uint16_t>(Op::_COUNT);

/// Canonical text name, e.g. "Add.I32", "GetProp.Dynamic", "Call.Direct".
/// Dots separate the base mnemonic from the specialization, per docs/ugb.md.
std::string_view opcode_name(Op op) noexcept;

/// Parses a text opcode. Accepts the canonical dotted form ("Add.I32") and the
/// uppercase underscore alias used throughout the T0 spec ("ADD_I32").
/// Returns Op::ILLEGAL on no match.
Op parse_opcode(std::string_view text) noexcept;

/// True if `op` is a speculative (typed) form with a canonical fallback.
bool is_speculative(Op op) noexcept;

/// Canonical form for a speculative opcode (Add.I32 -> Add.Any); identity for
/// canonical opcodes.
Op canonical_form(Op op) noexcept;

constexpr bool is_interop_call(Op op) noexcept {
    return op == Op::POLY_EXECUTE || op == Op::POLY_SEND;
}

constexpr bool is_branch(Op op) noexcept {
    return op == Op::JUMP || op == Op::JUMP_TRUE || op == Op::JUMP_FALSE ||
           op == Op::JUMP_EQ || op == Op::JUMP_NE;
}

constexpr bool is_call(Op op) noexcept {
    switch (op) {
    case Op::CALL_DIRECT:
    case Op::CALL_VIRTUAL:
    case Op::CALL_INTERFACE:
    case Op::CALL_DYNAMIC:
    case Op::CALL_CLOSURE:
    case Op::CALL_BUILTIN:
    case Op::CALL_FOREIGN:
    case Op::TAIL_CALL_DIRECT:
        return true;
    default:
        return false;
    }
}

}  // namespace vortex::ugb
