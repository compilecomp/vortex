#include "vortex/ugb/disassembler.hpp"

#include <cmath>
#include <set>
#include <sstream>

namespace vortex::ugb {

namespace {

// Opcodes whose dst operand is unused or redundant for text rendering.
bool dst_unused(Op op) {
    return op == Op::RETURN || op == Op::RETURN_UNIT || op == Op::UNREACHABLE ||
           is_branch(op) || op == Op::CHECK_NULL || op == Op::CHECK_NON_NULL ||
           op == Op::CHECK_CLASS || op == Op::CHECK_SHAPE ||
           op == Op::CHECK_BOUNDS || op == Op::SAFEPOINT_POLL ||
           op == Op::FENCE_SEQ_CST || op == Op::DEBUG_TRAP || op == Op::THROW ||
           op == Op::SWAP || op == Op::DEBUG_SRCPOS;
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

// Renders the metadata operand with token resolution where possible.
std::string render_meta(const UGBModule& m, const Instruction& ins) {
    std::ostringstream out;
    switch (ins.opcode) {
    case Op::CONST_I32:
        out << static_cast<int32_t>(ins.meta);
        break;
    case Op::CONST_I64: {
        if (ins.meta < m.constants.size() &&
            m.constants[ins.meta].kind == Constant::Kind::Int64) {
            out << m.constants[ins.meta].i64;
        } else {
            out << "<pool!" << ins.meta << ">";
        }
        break;
    }
    case Op::CONST_F64: {
        if (ins.meta < m.constants.size() &&
            m.constants[ins.meta].kind == Constant::Kind::Float64) {
            out << m.constants[ins.meta].f64;
        } else {
            out << "<pool!" << ins.meta << ">";
        }
        break;
    }
    case Op::CONST_STRING: {
        if (ins.meta < m.constants.size() &&
            m.constants[ins.meta].kind == Constant::Kind::String) {
            out << quoted(m.constants[ins.meta].str);
        } else {
            out << "<pool!" << ins.meta << ">";
        }
        break;
    }
    case Op::JUMP:
    case Op::JUMP_TRUE:
    case Op::JUMP_FALSE:
    case Op::JUMP_EQ:
    case Op::JUMP_NE:
        out << "@" << ins.meta;  // absolute bytecode target
        break;
    case Op::CALL_DIRECT:
    case Op::CALL_VIRTUAL:
    case Op::CALL_INTERFACE:
    case Op::CALL_DYNAMIC:
    case Op::CALL_CLOSURE:
    case Op::TAIL_CALL_DIRECT: {
        // Method tokens are 1-based over the module's method-token table.
        const uint32_t t = ins.meta;
        if (t >= 1 && t <= m.methods.size()) {
            out << m.methods[t - 1].name;
        } else {
            out << "<method!" << t << ">";
        }
        break;
    }
    case Op::CALL_BUILTIN: {
        if (ins.meta < m.builtins.size()) {
            out << m.builtins[ins.meta].name;
        } else {
            out << "<builtin!" << ins.meta << ">";
        }
        break;
    }
    case Op::NEW_OBJECT:
    case Op::CHECK_CLASS:
    case Op::CHECK_SHAPE: {
        if (ins.meta < m.classes.size()) {
            out << m.classes[ins.meta].name;
        } else {
            out << "<class!" << ins.meta << ">";
        }
        break;
    }
    case Op::GET_FIELD:
    case Op::GET_FIELD_SHAPE:
    case Op::SET_FIELD:
    case Op::SET_FIELD_SHAPE:
    case Op::GET_PROP_DYNAMIC:
    case Op::SET_PROP_DYNAMIC: {
        if (ins.meta < m.fields.size()) {
            out << m.fields[ins.meta].name;
        } else {
            out << "<field!" << ins.meta << ">";
        }
        break;
    }
    case Op::DEBUG_SRCPOS:
        out << "line " << ins.meta;
        break;
    default:
        out << ins.meta;
        break;
    }
    return out.str();
}

}  // namespace

std::string disassemble_method(const UGBModule& module, const UGBMethod& method) {
    std::ostringstream out;
    out << ".method " << method.name << "(regs=" << method.register_count
        << ", args=" << method.arg_count << ")\n";

    InstructionStream stream(method.code.data(), method.code.size());
    size_t offset = 0;
    std::set<uint32_t> targets;
    // Pre-collect branch targets to render labels for readability.
    {
        size_t off = 0;
        Instruction ins;
        while (off < method.code.size() && stream.decode_at(off, ins)) {
            if (is_branch(ins.opcode) && ins.has_meta) targets.insert(ins.meta);
        }
    }

    while (offset < method.code.size()) {
        if (targets.count(static_cast<uint32_t>(offset))) {
            out << "L" << offset << ":\n";
        }
        Instruction ins;
        if (!stream.decode_at(offset, ins)) {
            out << "  ; <malformed @ " << offset << ">\n";
            break;
        }
        out << "  " << opcode_name(ins.opcode);
        if (!dst_unused(ins.opcode)) {
            out << " v" << ins.dst;
        }
        for (uint16_t s : ins.srcs) out << ", v" << s;
        if (ins.has_meta) {
            out << ", " << render_meta(module, ins);
        }
        out << "\n";
    }
    out << ".end\n";
    return out.str();
}

std::string disassemble_module(const UGBModule& module) {
    std::ostringstream out;
    out << ".language \"" << module.language_name << "\"\n";
    for (const auto& c : module.classes) {
        out << ".class " << c.name << "\n";
    }
    for (const auto& f : module.fields) {
        if (f.klass_token < module.classes.size()) {
            out << ".field " << f.name << " in " << module.classes[f.klass_token].name
                << "\n";
        } else {
            out << ".field " << f.name << "\n";
        }
    }
    for (const auto& b : module.builtins) {
        out << ".builtin " << b.name << "\n";
    }
    for (const auto& m : module.method_table) {
        out << "\n" << disassemble_method(module, m);
    }
    return out.str();
}

}  // namespace vortex::ugb
