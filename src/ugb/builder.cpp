#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/encoding.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace vortex::ugb {

// ---------------------------------------------------------------------------
// MethodBuilder
// ---------------------------------------------------------------------------

MethodBuilder::MethodBuilder(UGBModule& module, std::string name,
                             uint16_t register_count, uint16_t arg_count)
    : module_(module) {
    method_.name = std::move(name);
    method_.register_count = register_count;
    method_.arg_count = arg_count;
    method_.id = static_cast<uint32_t>(module.method_table.size());
}

size_t MethodBuilder::emit(Op opcode, uint16_t dst,
                           std::initializer_list<uint16_t> srcs, bool has_meta,
                           uint32_t meta) {
    const size_t offset = code_.size();
    append_instruction(code_, opcode, dst, srcs, has_meta, meta);
    ++instruction_count_;
    return offset;
}

size_t MethodBuilder::const_i64(uint16_t dst, int64_t value) {
    const uint32_t pool = module_.intern_i64(value);
    return emit(Op::CONST_I64, dst, {}, true, pool);
}

size_t MethodBuilder::const_i32(uint16_t dst, int32_t value) {
    return emit(Op::CONST_I32, dst, {}, true, static_cast<uint32_t>(value));
}

size_t MethodBuilder::const_f64(uint16_t dst, double value) {
    const uint32_t pool = module_.intern_f64(value);
    return emit(Op::CONST_F64, dst, {}, true, pool);
}

size_t MethodBuilder::const_null(uint16_t dst) { return emit(Op::CONST_NULL, dst); }
size_t MethodBuilder::const_undefined(uint16_t dst) {
    return emit(Op::CONST_UNDEFINED, dst);
}
size_t MethodBuilder::const_true(uint16_t dst) { return emit(Op::CONST_TRUE, dst); }
size_t MethodBuilder::const_false(uint16_t dst) { return emit(Op::CONST_FALSE, dst); }

namespace {
// Byte offset of the u32 metadata slot inside an encoded instruction.
// Delegates to the single-source-of-truth layout constants (CEM-26 section 2:
// one definition per semantic domain, include/vortex/ugb/encoding.hpp).
constexpr size_t meta_offset(size_t instr_offset, size_t src_count) noexcept {
    return instr_offset + ugb::encoding::meta_offset(src_count);
}
}  // namespace

size_t MethodBuilder::jump(std::string_view label) {
    const size_t off = emit(Op::JUMP, 0, {}, true, 0);
    label_refs_.push_back({meta_offset(off, 0), std::string(label)});
    return off;
}

size_t MethodBuilder::jump_true(uint16_t cond, std::string_view label) {
    const size_t off = emit(Op::JUMP_TRUE, 0, {cond}, true, 0);
    label_refs_.push_back({meta_offset(off, 1), std::string(label)});
    return off;
}

size_t MethodBuilder::jump_false(uint16_t cond, std::string_view label) {
    const size_t off = emit(Op::JUMP_FALSE, 0, {cond}, true, 0);
    label_refs_.push_back({meta_offset(off, 1), std::string(label)});
    return off;
}

size_t MethodBuilder::call_direct(uint16_t dst_result, uint16_t arg_base,
                                  uint16_t argc, std::string_view callee) {
    const uint32_t token = module_.intern_method(std::string(callee));
    return emit(Op::CALL_DIRECT, dst_result, {arg_base, argc}, true, token);
}

size_t MethodBuilder::call_builtin(uint16_t dst_result, uint16_t arg_base,
                                   uint16_t argc, std::string_view builtin) {
    const uint32_t token = module_.intern_builtin(std::string(builtin));
    return emit(Op::CALL_BUILTIN, dst_result, {arg_base, argc}, true, token);
}

size_t MethodBuilder::ret(uint16_t value) { return emit(Op::RETURN, 0, {value}); }

void MethodBuilder::bind_label(std::string_view label) {
    labels_[std::string(label)] = code_.size();
}

Result<void> MethodBuilder::finish() {
    // Patch label references (u32 little-endian at the recorded offsets).
    for (const LabelRef& ref : label_refs_) {
        auto it = labels_.find(ref.label);
        if (it == labels_.end()) {
            return support::fail(support::ErrorCode::AssembleError,
                                 "undefined label '" + ref.label + "' in method '" +
                                     method_.name + "'");
        }
        const uint32_t target = static_cast<uint32_t>(it->second);
        uint8_t* p = code_.data() + ref.patch_offset;
        p[0] = static_cast<uint8_t>(target & 0xFF);
        p[1] = static_cast<uint8_t>((target >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((target >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((target >> 24) & 0xFF);
    }
    method_.code = std::move(code_);
    module_.method_table.push_back(std::move(method_));
    return support::ok();
}

// ---------------------------------------------------------------------------
// Text assembler
// ---------------------------------------------------------------------------
//
// Rule 65: no native exceptions anywhere in the core library. Assembler
// errors are recorded in an error-state struct and surfaced as Result.

namespace {

struct Tok {
    std::string text;
    uint32_t line = 0;
    bool end = false;  // sentinel token past end of stream (Rule 9: safe)
};

/// First-error-wins assembler error state. Helpers may be called with a
/// failed state; they short-circuit by returning sentinels that the caller
/// never uses because the enclosing phase checks `failed`.
struct AsmError {
    bool failed = false;
    std::string message;

    void fail(uint32_t line, const std::string& msg) {
        if (failed) return;
        failed = true;
        message = "line " + std::to_string(line) + ": " + msg;
    }
};

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}
bool is_ident(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
}

// Splits a line into tokens. Supports: identifiers (with dots), integers,
// floats, strings, ',', ':', '(', ')', '='.
std::vector<Tok> tokenize(const std::string& line, uint32_t line_no,
                          AsmError& err) {
    std::vector<Tok> out;
    size_t i = 0;
    while (i < line.size() && !err.failed) {
        const char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '#') break;  // comment
        if (c == ',' || c == ':' || c == '(' || c == ')' || c == '=') {
            out.push_back({std::string(1, c), line_no});
            ++i;
            continue;
        }
        if (c == '"') {
            const size_t start = ++i;
            while (i < line.size() && line[i] != '"') ++i;
            if (i >= line.size()) {
                err.fail(line_no, "unterminated string");
                break;
            }
            out.push_back({line.substr(start, i - start), line_no});
            ++i;
            continue;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            const size_t start = i;
            ++i;
            while (i < line.size() &&
                   (std::isdigit(static_cast<unsigned char>(line[i])) || line[i] == '.' ||
                    line[i] == 'e' || line[i] == 'E' || line[i] == '+' ||
                    line[i] == '-')) {
                // Stop '-'/'+'' after exponent only; simple numeric scan is fine
                // for the M0 assembler (no arithmetic expressions).
                if ((line[i] == '-' || line[i] == '+') &&
                    !(line[i - 1] == 'e' || line[i - 1] == 'E')) {
                    break;
                }
                ++i;
            }
            out.push_back({line.substr(start, i - start), line_no});
            continue;
        }
        if (is_ident_start(c)) {
            const size_t start = i;
            while (i < line.size() && is_ident(line[i])) ++i;
            out.push_back({line.substr(start, i - start), line_no});
            continue;
        }
        err.fail(line_no, std::string("unexpected character '") + c + "'");
    }
    return out;
}

uint16_t parse_register(const Tok& t, AsmError& err) {
    if (t.text.size() < 2 || (t.text[0] != 'v' && t.text[0] != 'V')) {
        err.fail(t.line, "expected register, got '" + t.text + "'");
        return 0;
    }
    const int v = std::atoi(t.text.c_str() + 1);
    if (v < 0 || v > 0xFFFF) {
        err.fail(t.line, "register out of range");
        return 0;
    }
    return static_cast<uint16_t>(v);
}

int64_t parse_int(const Tok& t, AsmError& err) {
    // strtoll with full-consumption check: no exceptions (Rule 65).
    if (t.text.empty()) {
        err.fail(t.line, "expected integer, got empty token");
        return 0;
    }
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(t.text.c_str(), &end, 10);
    if (errno == ERANGE || end != t.text.c_str() + t.text.size() ||
        end == t.text.c_str()) {
        err.fail(t.line, "expected integer, got '" + t.text + "'");
        return 0;
    }
    return static_cast<int64_t>(v);
}

double parse_float(const Tok& t, AsmError& err) {
    if (t.text.empty()) {
        err.fail(t.line, "expected float, got empty token");
        return 0.0;
    }
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(t.text.c_str(), &end);
    if (end != t.text.c_str() + t.text.size() || end == t.text.c_str()) {
        err.fail(t.line, "expected float, got '" + t.text + "'");
        return 0.0;
    }
    return v;
}

struct ParserCursor {
    const std::vector<Tok>* toks;
    size_t pos = 0;
    uint32_t line = 0;
    AsmError* err = nullptr;

    // Rule 9: assembler input is untrusted. Reads past the end return a
    // sentinel token instead of touching out-of-bounds memory.
    bool eof() const { return pos >= toks->size(); }
    const Tok& peek() const {
        static const Tok kEndTok{"", 0, true};
        return eof() ? kEndTok : (*toks)[pos];
    }
    Tok next() {
        if (eof()) return Tok{"", line, true};
        return (*toks)[pos++];
    }
    /// Reads one word argument, failing cleanly at end of stream.
    std::string expect_word(const char* what) {
        const Tok t = next();
        if (t.end) {
            err->fail(line, std::string("expected ") + what + " before end of line");
            return {};
        }
        return t.text;
    }
    bool take_if(const std::string& s) {
        if (!eof() && peek().text == s) {
            ++pos;
            return true;
        }
        return false;
    }
    void expect(const std::string& s) {
        if (eof() || peek().text != s) {
            err->fail(eof() ? line : peek().line, "expected '" + s + "'");
            return;
        }
        ++pos;
    }
};

}  // namespace

Result<UGBModule> assemble_module(std::string_view source) {
    UGBModule module;
    AsmError err;

    // Tokenize line by line to retain line numbers.
    std::vector<std::vector<Tok>> lines;
    {
        uint32_t line_no = 0;
        std::istringstream in{std::string(source)};
        std::string raw;
        while (std::getline(in, raw)) {
            ++line_no;
            auto toks = tokenize(raw, line_no, err);
            if (err.failed) break;
            if (!toks.empty()) lines.push_back(std::move(toks));
        }
    }

    std::unique_ptr<MethodBuilder> builder;
    bool in_method = false;

    for (auto& toks : lines) {
        if (err.failed) break;
        ParserCursor cur{&toks, 0, toks.empty() ? 0 : toks[0].line, &err};
        const Tok& head = cur.peek();

        if (head.text == ".language") {
            cur.next();
            module.language_name = cur.expect_word("a language name");
            continue;
        }
        if (head.text == ".class") {
            cur.next();
            module.intern_class(cur.expect_word("a class name"));
            continue;
        }
        if (head.text == ".field") {
            cur.next();
            const std::string fname = cur.expect_word("a field name");
            if (err.failed) break;
            cur.expect("in");
            if (err.failed) break;
            const std::string klass = cur.expect_word("a class name");
            if (err.failed) break;
            module.intern_field(fname, module.intern_class(klass));
            continue;
        }
        if (head.text == ".builtin") {
            cur.next();
            module.intern_builtin(cur.expect_word("a builtin name"));
            continue;
        }
        if (head.text == ".method") {
            if (in_method) err.fail(head.line, "nested .method");
            if (err.failed) break;
            cur.next();
            const std::string name = cur.expect_word("a method name");
            if (err.failed) break;
            uint16_t regs = 8, args = 0;
            if (cur.take_if("(")) {
                while (!cur.eof() && cur.peek().text != ")" && !err.failed) {
                    const std::string key = cur.next().text;
                    cur.expect("=");
                    if (err.failed) break;
                    const int64_t v = parse_int(cur.next(), err);
                    if (err.failed) break;
                    if (key == "regs") regs = static_cast<uint16_t>(v);
                    else if (key == "args") args = static_cast<uint16_t>(v);
                    else err.fail(head.line, "unknown method attribute " + key);
                    cur.take_if(",");
                }
                cur.expect(")");
            }
            if (err.failed) break;
            builder =
                std::make_unique<MethodBuilder>(module, name, regs, args);
            in_method = true;
            continue;
        }
        if (head.text == ".end") {
            cur.next();
            if (!in_method || !builder) err.fail(head.line, ".end without .method");
            if (err.failed) break;
            auto res = builder->finish();
            if (!res) return std::unexpected(res.error());
            builder.reset();
            in_method = false;
            continue;
        }

        // Label definition: "name:"
        if (cur.peek().text != "" && cur.pos + 1 < toks.size() &&
            toks[cur.pos + 1].text == ":") {
            if (!in_method) err.fail(head.line, "label outside .method");
            if (err.failed) break;
            builder->bind_label(head.text);
            cur.pos += 2;
            if (cur.eof()) continue;
        }

        // Instruction.
        if (!in_method) err.fail(head.line, "instruction outside .method");
        if (err.failed) break;
        const std::string op_text = cur.next().text;
        const Op op = parse_opcode(op_text);
        if (op == Op::ILLEGAL) {
            err.fail(head.line, "unknown opcode '" + op_text + "'");
            break;
        }

        // Collect comma-separated operands.
        std::vector<Tok> ops;
        if (!cur.eof()) {
            ops.push_back(cur.next());
            while (cur.take_if(",")) {
                if (cur.eof()) {
                    err.fail(head.line, "trailing comma in operand list");
                    break;
                }
                ops.push_back(cur.next());
            }
        }

        auto need = [&](size_t n) {
            if (ops.size() != n) {
                err.fail(head.line, "opcode " + op_text + " expects " +
                                        std::to_string(n) + " operands, got " +
                                        std::to_string(ops.size()));
            }
        };
        auto reg = [&](size_t i) { return parse_register(ops[i], err); };

        size_t off = 0;
        switch (op) {
        case Op::CONST_I32:
            need(2);
            if (!err.failed) {
                off = builder->const_i32(reg(0), static_cast<int32_t>(
                                  parse_int(ops[1], err)));
            }
            break;
        case Op::CONST_I64:
            need(2);
            if (!err.failed) off = builder->const_i64(reg(0), parse_int(ops[1], err));
            break;
        case Op::CONST_F64:
            need(2);
            if (!err.failed) off = builder->const_f64(reg(0), parse_float(ops[1], err));
            break;
        case Op::CONST_STRING:
            need(2);
            if (!err.failed) {
                off = builder->emit(op, reg(0), {}, true,
                                    module.intern_string(ops[1].text));
            }
            break;
        case Op::CONST_NULL: need(1); if (!err.failed) off = builder->const_null(reg(0)); break;
        case Op::CONST_TRUE: need(1); if (!err.failed) off = builder->const_true(reg(0)); break;
        case Op::CONST_FALSE: need(1); if (!err.failed) off = builder->const_false(reg(0)); break;
        case Op::CONST_UNDEFINED: need(1); if (!err.failed) off = builder->const_undefined(reg(0)); break;
        case Op::MOVE:
            need(2);
            if (!err.failed) off = builder->emit(op, reg(0), {reg(1)});
            break;
        case Op::ADD_ANY: case Op::ADD_I32: case Op::ADD_I64: case Op::ADD_F64:
        case Op::ADD_CHECKED_I32: case Op::ADD_CHECKED_I64:
        case Op::SUB_ANY: case Op::SUB_I32: case Op::SUB_I64: case Op::SUB_F64:
        case Op::SUB_CHECKED_I32:
        case Op::MUL_ANY: case Op::MUL_I32: case Op::MUL_I64: case Op::MUL_F64:
        case Op::MUL_CHECKED_I32:
        case Op::DIV_S_I64: case Op::DIV_U_I64: case Op::DIV_F64:
        case Op::REM_S_I64: case Op::REM_U_I64:
        case Op::AND_I: case Op::OR_I: case Op::XOR_I: case Op::SHL_I:
        case Op::SHR_S_I: case Op::SHR_U_I:
        case Op::EQ_I32: case Op::EQ_I64: case Op::EQ_F64: case Op::EQ_REF:
        case Op::NE_I64: case Op::NE_REF:
        case Op::LT_S_I64: case Op::LE_S_I64: case Op::GT_S_I64: case Op::GE_S_I64:
        case Op::LT_F64: case Op::LE_F64: case Op::GT_F64: case Op::GE_F64:
        case Op::EQ_ANY: case Op::COMPARE_ANY:
            need(3);
            if (!err.failed) off = builder->emit(op, reg(0), {reg(1), reg(2)});
            break;
        case Op::NEG_I64: case Op::NEG_F64: case Op::I64_TO_F64:
        case Op::F64_TO_I64: case Op::SEXT_I32_I64: case Op::TRUNC_I64_I32:
        case Op::BOX: case Op::UNBOX: case Op::ANY_TO_TYPED: case Op::TYPED_TO_ANY:
            need(2);
            if (!err.failed) off = builder->emit(op, reg(0), {reg(1)});
            break;
        case Op::RETURN:
            need(1);
            if (!err.failed) off = builder->ret(reg(0));
            break;
        case Op::RETURN_UNIT: case Op::UNREACHABLE: case Op::SAFEPOINT_POLL:
            need(0);
            off = builder->emit(op);
            break;
        case Op::JUMP:
            need(1);
            if (!err.failed) off = builder->jump(ops[0].text);
            break;
        case Op::JUMP_TRUE: case Op::JUMP_FALSE:
            need(2);
            if (!err.failed) {
                off = op == Op::JUMP_TRUE
                          ? builder->jump_true(reg(0), ops[1].text)
                          : builder->jump_false(reg(0), ops[1].text);
            }
            break;
        case Op::CALL_DIRECT: case Op::CALL_VIRTUAL: case Op::CALL_DYNAMIC:
            need(4);
            if (!err.failed) {
                off = op == Op::CALL_DIRECT
                          ? builder->call_direct(
                                reg(0), reg(1),
                                static_cast<uint16_t>(parse_int(ops[2], err)),
                                ops[3].text)
                          : builder->emit(op, reg(0), {reg(1),
                                                       static_cast<uint16_t>(parse_int(ops[2], err))},
                                          true,
                                          module.intern_method(ops[3].text));
            }
            break;
        case Op::CALL_BUILTIN:
            need(4);
            if (!err.failed) {
                off = builder->call_builtin(
                    reg(0), reg(1),
                    static_cast<uint16_t>(parse_int(ops[2], err)),
                    ops[3].text);
            }
            break;
        case Op::NEW_OBJECT:
            need(2);
            if (!err.failed) {
                off = builder->emit(op, reg(0), {}, true,
                                    module.intern_class(ops[1].text));
            }
            break;
        case Op::NEW_ARRAY:
            need(2);
            if (!err.failed) off = builder->emit(op, reg(0), {reg(1)});
            break;
        case Op::GET_FIELD: case Op::GET_FIELD_SHAPE: case Op::GET_PROP_DYNAMIC:
        case Op::SET_FIELD: case Op::SET_FIELD_SHAPE: case Op::SET_PROP_DYNAMIC: {
            need(3);
            if (err.failed) break;
            // dst/obj, obj/val, token-ref — token-ref is "fieldname" or
            // "Class.fieldname"; the owner class is optional.
            const std::string& ref = ops[2].text;
            uint32_t field_tok = 0;
            const size_t dot = ref.find('.');
            if (dot != std::string::npos) {
                const uint32_t klass_tok = module.intern_class(ref.substr(0, dot));
                field_tok = module.intern_field(ref.substr(dot + 1), klass_tok);
            } else {
                // Ownerless field token: use a reserved anonymous class token.
                field_tok = module.intern_field(ref, 0xFFFF);
            }
            if (op == Op::GET_FIELD || op == Op::GET_FIELD_SHAPE ||
                op == Op::GET_PROP_DYNAMIC) {
                // dst = result, src0 = object.
                off = builder->emit(op, reg(0), {reg(1)}, true, field_tok);
            } else {
                // SET_FIELD: src0 = object, src1 = value, dst unused.
                off = builder->emit(op, 0xFFFF, {reg(0), reg(1)}, true,
                                    field_tok);
            }
            break;
        }
        case Op::ARRAY_LENGTH:
            need(2);
            if (!err.failed) off = builder->emit(op, reg(0), {reg(1)});
            break;
        case Op::ARRAY_GET: case Op::ARRAY_GET_UNCHECKED:
            need(3);
            if (!err.failed) off = builder->emit(op, reg(0), {reg(1), reg(2)});
            break;
        case Op::ARRAY_SET: case Op::ARRAY_SET_UNCHECKED:
            need(3);
            // srcs = (array, index, value); dst unused.
            if (!err.failed) off = builder->emit(op, 0xFFFF, {reg(0), reg(1), reg(2)});
            break;
        case Op::CHECK_NULL: case Op::CHECK_NON_NULL:
            need(1);
            if (!err.failed) off = builder->emit(op, 0, {reg(0)});
            break;
        case Op::CHECK_CLASS: case Op::CHECK_SHAPE:
            need(2);
            if (!err.failed) {
                off = builder->emit(op, 0, {reg(0)}, true,
                                    module.intern_class(ops[1].text));
            }
            break;
        case Op::CHECK_BOUNDS:
            need(2);
            if (!err.failed) off = builder->emit(op, 0, {reg(0), reg(1)});
            break;
        case Op::DEBUG_SRCPOS:
            need(1);
            if (!err.failed) {
                off = builder->emit(op, 0, {}, true,
                                    static_cast<uint32_t>(parse_int(ops[0], err)));
            }
            break;
        case Op::DEBUG_TRAP:
            need(0);
            off = builder->emit(op);
            break;
        case Op::EXT_OP:
            need(2);
            if (!err.failed) {
                off = builder->emit(op, reg(0), {reg(1)}, true,
                                    static_cast<uint32_t>(parse_int(ops[1], err)));
            }
            break;
        default:
            err.fail(head.line, "opcode not supported by the M0 assembler: " +
                                    op_text);
        }
        if (err.failed) break;
        (void)off;
    }

    if (err.failed) {
        return support::fail(support::ErrorCode::AssembleError, err.message);
    }
    if (in_method) {
        return support::fail(support::ErrorCode::AssembleError,
                             "unterminated .method block");
    }
    return module;
}

}  // namespace vortex::ugb
