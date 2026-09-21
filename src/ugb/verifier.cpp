#include "vortex/ugb/verifier.hpp"

#include <set>
#include <sstream>

namespace vortex::ugb {

namespace {

std::string ctx(const UGBMethod& m, size_t offset, const std::string& msg) {
    std::ostringstream out;
    out << "verify: method '" << m.name << "' @+" << offset << ": " << msg;
    return out.str();
}

// Operand-role table (mirrors the conventions in include/vortex/ugb/opcode.hpp).
struct Shape {
    uint8_t srcs_min = 0;
    uint8_t srcs_max = 0;
    bool needs_meta = false;
};

Shape shape_of(Op op) {
    using S = Op;
    switch (op) {
    case S::CONST_NULL: case S::CONST_UNDEFINED: case S::CONST_FALSE:
    case S::CONST_TRUE:
        return {0, 0, false};
    case S::CONST_I32: case S::CONST_I64: case S::CONST_F64: case S::CONST_STRING:
    case S::CONST_METHOD: case S::NEW_OBJECT: case S::DEBUG_SRCPOS: case S::EXT_OP:
        return {0, 0, true};
    case S::MOVE: case S::SWAP: case S::COPY: case S::NEG_I64: case S::NEG_F64:
    case S::I64_TO_F64: case S::F64_TO_I64: case S::SEXT_I32_I64:
    case S::TRUNC_I64_I32: case S::BOX: case S::UNBOX: case S::ANY_TO_TYPED:
    case S::TYPED_TO_ANY: case S::ARRAY_LENGTH: case S::NEW_ARRAY:
        return {1, 1, false};
    case S::ADD_ANY: case S::ADD_I32: case S::ADD_I64: case S::ADD_F64:
    case S::ADD_CHECKED_I32: case S::ADD_CHECKED_I64:
    case S::SUB_ANY: case S::SUB_I32: case S::SUB_I64: case S::SUB_F64:
    case S::SUB_CHECKED_I32:
    case S::MUL_ANY: case S::MUL_I32: case S::MUL_I64: case S::MUL_F64:
    case S::MUL_CHECKED_I32:
    case S::DIV_S_I64: case S::DIV_U_I64: case S::DIV_F64:
    case S::REM_S_I64: case S::REM_U_I64:
    case S::AND_I: case S::OR_I: case S::XOR_I: case S::SHL_I: case S::SHR_S_I:
    case S::SHR_U_I:
    case S::EQ_I32: case S::EQ_I64: case S::EQ_F64: case S::EQ_REF: case S::NE_I64:
    case S::NE_REF: case S::LT_S_I64: case S::LE_S_I64: case S::GT_S_I64:
    case S::GE_S_I64: case S::LT_F64: case S::LE_F64: case S::GT_F64:
    case S::GE_F64: case S::EQ_ANY: case S::COMPARE_ANY:
        return {2, 2, false};
    case S::JUMP:
        return {0, 0, true};
    case S::JUMP_TRUE: case S::JUMP_FALSE: case S::JUMP_EQ: case S::JUMP_NE:
    case S::RETURN:
        return {1, 1, false};
    case S::RETURN_UNIT: case S::UNREACHABLE: case S::SAFEPOINT_POLL:
    case S::FENCE_SEQ_CST: case S::DEBUG_TRAP:
        return {0, 0, false};
    case S::CALL_DIRECT: case S::CALL_VIRTUAL: case S::CALL_INTERFACE:
    case S::CALL_DYNAMIC: case S::CALL_CLOSURE: case S::CALL_BUILTIN:
    case S::CALL_FOREIGN: case S::TAIL_CALL_DIRECT:
        return {2, 2, true};  // arg_base, argc + callee token
    case S::GET_FIELD: case S::GET_FIELD_SHAPE: case S::GET_PROP_DYNAMIC:
        return {1, 1, true};
    case S::SET_FIELD: case S::SET_FIELD_SHAPE: case S::SET_PROP_DYNAMIC:
        return {2, 2, true};
    case S::DELETE_PROP_DYNAMIC: case S::HAS_PROP_DYNAMIC:
        return {1, 1, true};
    case S::ARRAY_GET: case S::ARRAY_GET_UNCHECKED:
        return {2, 2, false};
    case S::ARRAY_SET: case S::ARRAY_SET_UNCHECKED:
        return {3, 3, false};
    case S::CHECK_NULL: case S::CHECK_NON_NULL:
        return {1, 1, false};
    case S::CHECK_CLASS: case S::CHECK_SHAPE:
        return {1, 1, true};
    case S::CHECK_BOUNDS:
        return {2, 2, false};
    default:
        return {0, 8, false};
    }
}

}  // namespace

Result<void> verify_method(const UGBModule& module, const UGBMethod& method) {
    InstructionStream stream(method.code.data(), method.code.size());
    size_t offset = 0;

    // Branch targets, for a second validation pass.
    std::set<uint32_t> branch_targets;
    std::vector<uint32_t> branch_sources;

    // Pass 1: structural validity.
    while (offset < method.code.size()) {
        Instruction ins;
        if (!stream.decode_at(offset, ins)) {
            return support::fail(support::ErrorCode::VerifyError,
                                 ctx(method, offset, "malformed instruction"));
        }
        const uint16_t op_idx = static_cast<uint16_t>(ins.opcode);
        if (ins.opcode == Op::ILLEGAL || op_idx >= kOpcodeCount) {
            return support::fail(support::ErrorCode::VerifyError,
                                 ctx(method, offset, "invalid opcode"));
        }
        const Shape s = shape_of(ins.opcode);
        if (ins.srcs.size() < s.srcs_min || ins.srcs.size() > s.srcs_max) {
            return support::fail(
                support::ErrorCode::VerifyError,
                ctx(method, offset, "operand count out of range for " +
                                        std::string(opcode_name(ins.opcode))));
        }
        if (s.needs_meta && !ins.has_meta) {
            return support::fail(support::ErrorCode::VerifyError,
                                 ctx(method, offset,
                                     "missing metadata token for " +
                                         std::string(opcode_name(ins.opcode))));
        }
        // Call argument window: srcs = (arg_base, argc) and the callee frame
        // reads R[arg_base .. arg_base+argc-1]. argc is NOT a register, so
        // calls are validated here and skipped by the generic srcs check.
        if (is_call(ins.opcode)) {
            if (ins.srcs.size() != 2) {
                return support::fail(support::ErrorCode::VerifyError,
                                     ctx(method, offset, "call without (arg_base, argc)"));
            }
            const uint32_t base = ins.srcs[0];
            const uint32_t argc = ins.srcs[1];
            if (base >= method.register_count ||
                base + argc > method.register_count) {
                return support::fail(
                    support::ErrorCode::VerifyError,
                    ctx(method, offset, "call argument window out of range"));
            }
            // Callee token must resolve against the module's method/builtin
            // token table.
            if (ins.opcode == Op::CALL_BUILTIN) {
                if (ins.meta >= module.builtins.size()) {
                    return support::fail(support::ErrorCode::VerifyError,
                                         ctx(method, offset, "builtin token out of range"));
                }
            } else if (ins.meta < 1 || ins.meta > module.methods.size()) {
                return support::fail(support::ErrorCode::VerifyError,
                                     ctx(method, offset, "method token out of range"));
            }
        } else {
            for (uint16_t r : ins.srcs) {
                if (r >= method.register_count) {
                    return support::fail(support::ErrorCode::VerifyError,
                                         ctx(method, offset, "source register out of range"));
                }
            }
        }
        // Token-range validation against the module tables.
        switch (ins.opcode) {
        case Op::CONST_I64:
        case Op::CONST_F64:
        case Op::CONST_STRING:
            if (ins.meta >= module.constants.size()) {
                return support::fail(support::ErrorCode::VerifyError,
                                     ctx(method, offset, "constant pool index out of range"));
            }
            break;
        case Op::NEW_OBJECT:
        case Op::CHECK_CLASS:
        case Op::CHECK_SHAPE:
            if (ins.meta >= module.classes.size()) {
                return support::fail(support::ErrorCode::VerifyError,
                                     ctx(method, offset, "class token out of range"));
            }
            break;
        case Op::GET_FIELD:
        case Op::GET_FIELD_SHAPE:
        case Op::SET_FIELD:
        case Op::SET_FIELD_SHAPE:
        case Op::GET_PROP_DYNAMIC:
        case Op::SET_PROP_DYNAMIC:
            if (ins.meta >= module.fields.size()) {
                return support::fail(support::ErrorCode::VerifyError,
                                     ctx(method, offset, "field token out of range"));
            }
            break;
        default:
            break;
        }
        if (ins.dst != 0xFFFF && ins.dst >= method.register_count) {
            return support::fail(support::ErrorCode::VerifyError,
                                 ctx(method, offset, "destination register out of range"));
        }
        if (is_branch(ins.opcode)) {
            if (!ins.has_meta) {
                return support::fail(support::ErrorCode::VerifyError,
                                     ctx(method, offset, "branch without target"));
            }
            branch_targets.insert(ins.meta);
            branch_sources.push_back(static_cast<uint32_t>(offset));
        }
        // Calls and branches are terminators only for direct flow analysis when
        // followed by RETURN; full CFG liveness is a J2+ concern, so M0 checks
        // only that the method contains at least one terminator.
    }

    bool has_return = false;
    offset = 0;
    while (offset < method.code.size()) {
        Instruction ins;
        if (!stream.decode_at(offset, ins)) break;
        if (ins.opcode == Op::RETURN || ins.opcode == Op::RETURN_UNIT ||
            ins.opcode == Op::TAIL_CALL_DIRECT) {
            has_return = true;
            break;
        }
    }
    if (!has_return) {
        return support::fail(support::ErrorCode::VerifyError,
                             ctx(method, 0, "no return / tail call terminator"));
    }

    // Pass 2: every branch target must land exactly on an instruction start.
    // Walk instruction starts from offset 0 and mark them, then check targets.
    std::set<uint32_t> instruction_starts;
    offset = 0;
    while (offset < method.code.size()) {
        instruction_starts.insert(static_cast<uint32_t>(offset));
        Instruction ins;
        if (!stream.decode_at(offset, ins)) break;
    }
    for (uint32_t target : branch_targets) {
        if (target >= method.code.size() || instruction_starts.count(target) == 0) {
            return support::fail(support::ErrorCode::VerifyError,
                                 ctx(method, target,
                                     "branch target not on instruction start"));
        }
    }

    return support::ok();
}

Result<void> verify_module(const UGBModule& module) {
    for (const UGBMethod& m : module.method_table) {
        auto res = verify_method(module, m);
        if (!res) return res;
    }
    return support::ok();
}

}  // namespace vortex::ugb
