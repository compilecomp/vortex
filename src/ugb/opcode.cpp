#include "vortex/ugb/opcode.hpp"

#include <array>
#include <cctype>
#include <string>
#include <unordered_map>

namespace vortex::ugb {

namespace {

struct OpNameEntry {
    Op op;
    const char* name;
};

// Canonical dotted names (docs/ugb.md style: "Add.I32", "GetProp.Dynamic").
constexpr std::array<OpNameEntry, static_cast<size_t>(Op::_COUNT)> kNames{{
    {Op::ILLEGAL, "Illegal"},
    {Op::CONST_NULL, "Const.Null"},
    {Op::CONST_UNDEFINED, "Const.Undefined"},
    {Op::CONST_FALSE, "Const.False"},
    {Op::CONST_TRUE, "Const.True"},
    {Op::CONST_I32, "Const.I32"},
    {Op::CONST_I64, "Const.I64"},
    {Op::CONST_F64, "Const.F64"},
    {Op::CONST_STRING, "Const.String"},
    {Op::CONST_METHOD, "Const.Method"},
    {Op::MOVE, "Move"},
    {Op::SWAP, "Swap"},
    {Op::COPY, "Copy"},
    {Op::ADD_ANY, "Add.Any"},
    {Op::ADD_I32, "Add.I32"},
    {Op::ADD_I64, "Add.I64"},
    {Op::ADD_F64, "Add.F64"},
    {Op::ADD_CHECKED_I32, "Add.Checked.I32"},
    {Op::ADD_CHECKED_I64, "Add.Checked.I64"},
    {Op::SUB_ANY, "Sub.Any"},
    {Op::SUB_I32, "Sub.I32"},
    {Op::SUB_I64, "Sub.I64"},
    {Op::SUB_F64, "Sub.F64"},
    {Op::SUB_CHECKED_I32, "Sub.Checked.I32"},
    {Op::MUL_ANY, "Mul.Any"},
    {Op::MUL_I32, "Mul.I32"},
    {Op::MUL_I64, "Mul.I64"},
    {Op::MUL_F64, "Mul.F64"},
    {Op::MUL_CHECKED_I32, "Mul.Checked.I32"},
    {Op::DIV_S_I64, "Div.S.I64"},
    {Op::DIV_U_I64, "Div.U.I64"},
    {Op::DIV_F64, "Div.F64"},
    {Op::REM_S_I64, "Rem.S.I64"},
    {Op::REM_U_I64, "Rem.U.I64"},
    {Op::NEG_I64, "Neg.I64"},
    {Op::NEG_F64, "Neg.F64"},
    {Op::AND_I, "And.I"},
    {Op::OR_I, "Or.I"},
    {Op::XOR_I, "Xor.I"},
    {Op::SHL_I, "Shl.I"},
    {Op::SHR_S_I, "Shr.S.I"},
    {Op::SHR_U_I, "Shr.U.I"},
    {Op::EQ_I32, "Eq.I32"},
    {Op::EQ_I64, "Eq.I64"},
    {Op::EQ_F64, "Eq.F64"},
    {Op::EQ_REF, "Eq.Ref"},
    {Op::EQ_NULL, "Eq.Null"},
    {Op::NE_I64, "Ne.I64"},
    {Op::NE_REF, "Ne.Ref"},
    {Op::LT_S_I64, "Lt.S.I64"},
    {Op::LE_S_I64, "Le.S.I64"},
    {Op::GT_S_I64, "Gt.S.I64"},
    {Op::GE_S_I64, "Ge.S.I64"},
    {Op::LT_F64, "Lt.F64"},
    {Op::LE_F64, "Le.F64"},
    {Op::GT_F64, "Gt.F64"},
    {Op::GE_F64, "Ge.F64"},
    {Op::EQ_ANY, "Eq.Any"},
    {Op::COMPARE_ANY, "Compare.Any"},
    {Op::I64_TO_F64, "IntToFloat.I64.F64"},
    {Op::F64_TO_I64, "FloatToInt.F64.I64"},
    {Op::SEXT_I32_I64, "SExt.I32.I64"},
    {Op::TRUNC_I64_I32, "Trunc.I64.I32"},
    {Op::BOX, "Box"},
    {Op::UNBOX, "Unbox"},
    {Op::ANY_TO_TYPED, "AnyToTyped"},
    {Op::TYPED_TO_ANY, "TypedToAny"},
    {Op::JUMP, "Jump"},
    {Op::JUMP_TRUE, "JumpTrue"},
    {Op::JUMP_FALSE, "JumpFalse"},
    {Op::JUMP_EQ, "JumpEq"},
    {Op::JUMP_NE, "JumpNe"},
    {Op::RETURN, "Return"},
    {Op::RETURN_UNIT, "Return.Unit"},
    {Op::UNREACHABLE, "Unreachable"},
    {Op::SWITCH_INT, "Switch.Int"},
    {Op::CALL_DIRECT, "Call.Direct"},
    {Op::CALL_VIRTUAL, "Call.Virtual"},
    {Op::CALL_INTERFACE, "Call.Interface"},
    {Op::CALL_DYNAMIC, "Call.Dynamic"},
    {Op::CALL_CLOSURE, "Call.Closure"},
    {Op::CALL_BUILTIN, "Call.Builtin"},
    {Op::CALL_FOREIGN, "Call.Foreign"},
    {Op::TAIL_CALL_DIRECT, "TailCall.Direct"},
    {Op::NEW_OBJECT, "New.Object"},
    {Op::NEW_ARRAY, "New.Array"},
    {Op::GET_FIELD, "GetField"},
    {Op::SET_FIELD, "SetField"},
    {Op::GET_FIELD_SHAPE, "GetField.Shape"},
    {Op::SET_FIELD_SHAPE, "SetField.Shape"},
    {Op::GET_PROP_DYNAMIC, "GetProp.Dynamic"},
    {Op::SET_PROP_DYNAMIC, "SetProp.Dynamic"},
    {Op::DELETE_PROP_DYNAMIC, "DeleteProp.Dynamic"},
    {Op::HAS_PROP_DYNAMIC, "HasProp.Dynamic"},
    {Op::ARRAY_LENGTH, "Array.Length"},
    {Op::ARRAY_GET, "Array.Get"},
    {Op::ARRAY_SET, "Array.Set"},
    {Op::ARRAY_GET_UNCHECKED, "Array.Get.Unchecked"},
    {Op::ARRAY_SET_UNCHECKED, "Array.Set.Unchecked"},
    {Op::CHECK_NULL, "Check.Null"},
    {Op::CHECK_NON_NULL, "Check.NonNull"},
    {Op::CHECK_CLASS, "Check.Class"},
    {Op::CHECK_BOUNDS, "Check.Bounds"},
    {Op::CHECK_SHAPE, "Check.Shape"},
    {Op::ATOMIC_LOAD, "Atomic.Load"},
    {Op::ATOMIC_STORE, "Atomic.Store"},
    {Op::FENCE_SEQ_CST, "Fence.SeqCst"},
    {Op::SAFEPOINT_POLL, "Safepoint"},
    {Op::CLOSURE_NEW, "Closure.New"},
    {Op::CLOSURE_GET_UPVALUE, "Closure.GetUpvalue"},
    {Op::CLOSURE_SET_UPVALUE, "Closure.SetUpvalue"},
    {Op::TRY_BEGIN, "TryBegin"},
    {Op::TRY_END, "TryEnd"},
    {Op::THROW, "Throw"},
    {Op::WRITE_BARRIER_STORE, "WriteBarrier.Store"},
    {Op::DEBUG_SRCPOS, "Debug.SourcePosition"},
    {Op::DEBUG_TRAP, "Debug.Trap"},
    {Op::EXT_OP, "ExtOp"},
}};

std::string normalize_alias(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '.') c = '_';
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

// Alias map: normalized uppercase-underscore name -> opcode. Also registers the
// dotted form normalized, so both "Add.I32" and "ADD_I32" resolve.
const std::unordered_map<std::string, Op>& alias_map() {
    static const std::unordered_map<std::string, Op> map = [] {
        std::unordered_map<std::string, Op> m;
        m.reserve(kNames.size() * 2);
        for (const auto& e : kNames) {
            m[normalize_alias(e.name)] = e.op;
        }
        // Extra aliases used in the tier docs.
        m.emplace("CHECK_CLASS", Op::CHECK_CLASS);
        m.emplace("CHECK_KLASS", Op::CHECK_CLASS);
        return m;
    }();
    return map;
}

}  // namespace

std::string_view opcode_name(Op op) noexcept {
    const uint16_t i = static_cast<uint16_t>(op);
    if (i < kOpcodeCount) return kNames[i].name;
    return "Illegal";
}

Op parse_opcode(std::string_view text) noexcept {
    static const auto& map = alias_map();
    auto it = map.find(normalize_alias(text));
    if (it == map.end()) return Op::ILLEGAL;
    return it->second;
}

bool is_speculative(Op op) noexcept {
    switch (op) {
    case Op::ADD_I32:
    case Op::ADD_I64:
    case Op::ADD_F64:
    case Op::ADD_CHECKED_I32:
    case Op::ADD_CHECKED_I64:
    case Op::SUB_I32:
    case Op::SUB_I64:
    case Op::SUB_F64:
    case Op::SUB_CHECKED_I32:
    case Op::MUL_I32:
    case Op::MUL_I64:
    case Op::MUL_F64:
    case Op::MUL_CHECKED_I32:
    case Op::GET_FIELD_SHAPE:
    case Op::SET_FIELD_SHAPE:
    case Op::CHECK_CLASS:
    case Op::CHECK_SHAPE:
        return true;
    default:
        return false;
    }
}

Op canonical_form(Op op) noexcept {
    switch (op) {
    case Op::ADD_I32:
    case Op::ADD_I64:
    case Op::ADD_F64:
    case Op::ADD_CHECKED_I32:
    case Op::ADD_CHECKED_I64:
        return Op::ADD_ANY;
    case Op::SUB_I32:
    case Op::SUB_I64:
    case Op::SUB_F64:
    case Op::SUB_CHECKED_I32:
        return Op::SUB_ANY;
    case Op::MUL_I32:
    case Op::MUL_I64:
    case Op::MUL_F64:
    case Op::MUL_CHECKED_I32:
        return Op::MUL_ANY;
    case Op::GET_FIELD_SHAPE:
        return Op::GET_FIELD;
    case Op::SET_FIELD_SHAPE:
        return Op::SET_FIELD;
    default:
        return op;
    }
}

}  // namespace vortex::ugb
