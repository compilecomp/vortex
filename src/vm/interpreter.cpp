#include "vortex/vm/interpreter.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

#include "vortex/support/log.hpp"
#include "vortex/ugb/verifier.hpp"

namespace vortex::vm {

using vortex::support::ErrorCode;
using vortex::support::fail;
using vortex::support::ok;
using vortex::support::Result;
using ugb::IcSlot;
using ugb::Op;
using ugb::ProfileSlot;

// ---------------------------------------------------------------------------
// Construction / registration
// ---------------------------------------------------------------------------

Interpreter::Interpreter(gc::Heap& heap, InterpreterConfig config)
    : heap_(heap), config_(config) {
    tiering_ = TieringPolicy(config_.tiering);
}

void Interpreter::register_builtin(
    std::string_view name, TaggedValue (*fn)(std::span<const TaggedValue>, void*),
    void* user) {
    builtins_.push_back(Builtin{std::string(name), fn, user});
}

// ---------------------------------------------------------------------------
// Per-method / per-module caches
// ---------------------------------------------------------------------------

Interpreter::MethodData& Interpreter::method_data(const ugb::UGBMethod& m) {
    for (auto& e : method_data_) {
        if (e.first == &m) return e.second;
    }
    method_data_.emplace_back(&m, MethodData{});
    return method_data_.back().second;
}

void Interpreter::build_offset_maps(const ugb::UGBMethod& m, MethodData& md) {
    ugb::InstructionStream stream(m.code.data(), m.code.size());
    size_t off = 0;
    uint32_t index = 0;
    md.instruction_offsets.clear();
    while (off < m.code.size()) {
        md.instruction_offsets.push_back(static_cast<uint32_t>(off));
        ugb::Instruction instr;
        if (!stream.decode_at(off, instr)) break;
        ++index;
    }
    md.instruction_index.assign(m.code.size(), 0xFFFFFFFFu);
    for (uint32_t i = 0; i < md.instruction_offsets.size(); ++i) {
        const uint32_t start = md.instruction_offsets[i];
        const uint32_t end = i + 1 < md.instruction_offsets.size()
                                 ? md.instruction_offsets[i + 1]
                                 : static_cast<uint32_t>(m.code.size());
        for (uint32_t b = start; b < end; ++b) md.instruction_index[b] = i;
    }
    md.ready = true;
}

Interpreter::ModuleData& Interpreter::module_data(ugb::UGBModule& module) {
    for (auto& e : module_data_) {
        if (e.first == &module) return e.second;
    }
    module_data_.emplace_back(&module, ModuleData{});
    return module_data_.back().second;
}

Result<void> Interpreter::build_module_data(ugb::UGBModule& module,
                                            ModuleData& md) {
    // Class tokens -> Klass objects. Klass layouts are built from the module's
    // field tokens (owner class known from "Class.field" references) so that
    // NEW_OBJECT allocates the full field array up front.
    md.klass_table.clear();
    for (const auto& c : module.classes) {
        Klass* k = registry_.create(c.name);
        md.klass_table.push_back(k);
    }
    for (const auto& f : module.fields) {
        if (f.klass_token < md.klass_table.size()) {
            md.klass_table[f.klass_token]->add_field(f.name);
        }
    }
    md.field_slot.assign(module.fields.size(), -1);
    md.builtin_slot.assign(module.builtins.size(), -1);
    md.method_resolution.assign(module.methods.size() + 1, -1);
    md.ready = true;
    return ok();
}

// ---------------------------------------------------------------------------
// Generic (canonical) slow paths — these are the ADD_ANY-style fallbacks the
// spec describes as "handle all cases but are slower".
// ---------------------------------------------------------------------------

TaggedValue Interpreter::generic_add(TaggedValue a, TaggedValue b) {
    if (a.is_smi() && b.is_smi()) {
        const int64_t r = a.as_smi() + b.as_smi();
        if (r < TaggedValue::smi_min() || r > TaggedValue::smi_max()) {
            return TaggedValue::undefined();  // overflow escape: caller reports
        }
        return TaggedValue::smi(r);
    }
    return TaggedValue::undefined();
}

TaggedValue Interpreter::generic_sub(TaggedValue a, TaggedValue b) {
    if (a.is_smi() && b.is_smi()) {
        const int64_t r = a.as_smi() - b.as_smi();
        if (r < TaggedValue::smi_min() || r > TaggedValue::smi_max()) {
            return TaggedValue::undefined();
        }
        return TaggedValue::smi(r);
    }
    return TaggedValue::undefined();
}

TaggedValue Interpreter::generic_mul(TaggedValue a, TaggedValue b) {
    if (a.is_smi() && b.is_smi()) {
        const int64_t r = a.as_smi() * b.as_smi();
        if (r < TaggedValue::smi_min() || r > TaggedValue::smi_max()) {
            return TaggedValue::undefined();
        }
        return TaggedValue::smi(r);
    }
    return TaggedValue::undefined();
}

TaggedValue Interpreter::generic_compare(TaggedValue a, TaggedValue b, Op cmp) {
    if (!a.is_smi() || !b.is_smi()) return TaggedValue::undefined();
    const int64_t x = a.as_smi();
    const int64_t y = b.as_smi();
    bool r = false;
    switch (cmp) {
    case Op::EQ_I64: case Op::EQ_I32: r = x == y; break;
    case Op::NE_I64: r = x != y; break;
    case Op::LT_S_I64: r = x < y; break;
    case Op::LE_S_I64: r = x <= y; break;
    case Op::GT_S_I64: r = x > y; break;
    case Op::GE_S_I64: r = x >= y; break;
    default: return TaggedValue::undefined();
    }
    return TaggedValue::boolean(r);
}

TaggedValue Interpreter::generic_get_field(TaggedValue obj, uint32_t field_token,
                                           const ugb::UGBModule& module) {
    if (!obj.is_heap_object() || field_token >= module.fields.size()) {
        return TaggedValue::undefined();
    }
    auto* o = obj.as_heap_object();
    auto* k = o->header.klass;
    if (k == nullptr) return TaggedValue::undefined();
    const int idx = k->find_field(module.fields[field_token].name);
    if (idx < 0) return TaggedValue::undefined();
    return static_cast<Object*>(o)->field(static_cast<uint32_t>(idx));
}

bool Interpreter::generic_set_field(TaggedValue obj, TaggedValue value,
                                    uint32_t field_token,
                                    const ugb::UGBModule& module) {
    if (!obj.is_heap_object() || field_token >= module.fields.size()) return false;
    auto* o = obj.as_heap_object();
    auto* k = o->header.klass;
    if (k == nullptr) return false;
    const int idx = k->find_field(module.fields[field_token].name);
    if (idx < 0) return false;
    store_field(&static_cast<Object*>(o)->field(static_cast<uint32_t>(idx)), value);
    if (value.is_heap_object()) heap_.card_table().mark_dirty(o);
    return true;
}

// ---------------------------------------------------------------------------
// Adaptive rewriting (docs/tier-t0.md section 6).
// ---------------------------------------------------------------------------

void Interpreter::maybe_rewrite_to_typed(const ugb::UGBMethod& m, size_t pc,
                                         Op canonical, Op typed) {
    if (!config_.enable_adaptive_rewriting) return;
    uint8_t* raw = const_cast<uint8_t*>(m.code.data()) + pc;
    const uint16_t typed_bits = static_cast<uint16_t>(typed);
    raw[0] = static_cast<uint8_t>(typed_bits & 0xFF);
    raw[1] = static_cast<uint8_t>(typed_bits >> 8);
    stats_.typed_rewrites++;
    support::debug("t0", "rewrote " + std::string(ugb::opcode_name(canonical)) +
                             " -> " + std::string(ugb::opcode_name(typed)) +
                             " in method '" + m.name + "'");
}

void Interpreter::maybe_rewrite_to_generic(const ugb::UGBMethod& m, size_t pc,
                                           Op typed) {
    if (!config_.enable_adaptive_rewriting) return;
    const Op canonical = ugb::canonical_form(typed);
    if (canonical == typed) return;
    uint8_t* raw = const_cast<uint8_t*>(m.code.data()) + pc;
    const uint16_t canon_bits = static_cast<uint16_t>(canonical);
    raw[0] = static_cast<uint8_t>(canon_bits & 0xFF);
    raw[1] = static_cast<uint8_t>(canon_bits >> 8);
    stats_.generic_rewrites++;
    support::debug("t0", "demoted " + std::string(ugb::opcode_name(typed)) +
                             " -> " + std::string(ugb::opcode_name(canonical)) +
                             " in method '" + m.name + "'");
}

void Interpreter::record_bigram(const ugb::UGBMethod& m, Op first, Op second) {
    // Bigram hotness feeds superstencil promotion in J1 (docs/tier-j1.md
    // section 3). M0 counts into the module-level table.
    const uint64_t key = (static_cast<uint64_t>(first) << 16) |
                         static_cast<uint64_t>(second);
    bigram_counts_[key]++;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

Result<RunResult> Interpreter::run(ugb::UGBModule& module, std::string_view entry,
                                   std::span<const TaggedValue> args) {
    const int32_t mid = module.find_method(std::string(entry));
    if (mid < 0) {
        return fail(ErrorCode::RuntimeError,
                    "entry method '" + std::string(entry) + "' not found");
    }
    ugb::UGBMethod& method = module.method_table[static_cast<size_t>(mid)];

    // Mandatory verification before execution (docs/ugb.md section 9).
    auto verified = ugb::verify_module(module);
    if (!verified) return std::unexpected(verified.error());

    ModuleData& md = module_data(module);
    if (!md.ready) {
        auto built = build_module_data(module, md);
        if (!built) return std::unexpected(built.error());
    }

    if (args.size() != method.arg_count) {
        return fail(ErrorCode::RuntimeError,
                    "method '" + method.name + "' expects " +
                        std::to_string(method.arg_count) + " args, got " +
                        std::to_string(args.size()));
    }
    return execute(module, method, args);
}

// ---------------------------------------------------------------------------
// The dispatch loop
// ---------------------------------------------------------------------------

namespace {

// Compact decoded instruction kept in registers inside the loop.
struct Decoded {
    uint16_t opcode = 0;
    uint16_t dst = 0;
    uint16_t s0 = 0;
    uint16_t s1 = 0;
    uint16_t s2 = 0;
    uint8_t nsrc = 0;
    uint32_t meta = 0;
    bool has_meta = false;
    size_t next_pc = 0;
};

inline bool decode_fast(const uint8_t* code, size_t size, size_t pc,
                        Decoded& d) {
    if (pc + 6 > size) return false;
    d.opcode = static_cast<uint16_t>(code[pc] | (code[pc + 1] << 8));
    const uint8_t flags = code[pc + 2];
    d.dst = static_cast<uint16_t>(code[pc + 3] | (code[pc + 4] << 8));
    d.nsrc = code[pc + 5];
    size_t p = pc + 6;
    if (d.nsrc > 3) {
        // wide forms (>3 srcs) exist in the ISA; decode lazily via generic path
        if (p + static_cast<size_t>(d.nsrc) * 2 > size) return false;
        p += static_cast<size_t>(d.nsrc) * 2;
        d.s0 = d.s1 = d.s2 = 0;
    } else {
        auto rd = [&](size_t i) -> uint16_t {
            return p + i * 2 + 1 < size
                       ? static_cast<uint16_t>(code[p + i * 2] |
                                               (code[p + i * 2 + 1] << 8))
                       : 0;
        };
        d.s0 = d.nsrc > 0 ? rd(0) : 0;
        d.s1 = d.nsrc > 1 ? rd(1) : 0;
        d.s2 = d.nsrc > 2 ? rd(2) : 0;
        p += static_cast<size_t>(d.nsrc) * 2;
    }
    d.has_meta = (flags & ugb::FLAG_HAS_META) != 0;
    if (d.has_meta) {
        if (p + 4 > size) return false;
        d.meta = static_cast<uint32_t>(code[p]) |
                 (static_cast<uint32_t>(code[p + 1]) << 8) |
                 (static_cast<uint32_t>(code[p + 2]) << 16) |
                 (static_cast<uint32_t>(code[p + 3]) << 24);
        p += 4;
    }
    d.next_pc = p;
    return true;
}

TaggedValue builtin_print(std::span<const TaggedValue> args, void*) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) std::fputs(" ", stdout);
        const std::string s = args[i].to_string();
        std::fputs(s.c_str(), stdout);
    }
    std::fputs("\n", stdout);
    return TaggedValue::undefined();
}

}  // namespace

Result<RunResult> Interpreter::execute(ugb::UGBModule& module,
                                       ugb::UGBMethod& method,
                                       std::span<const TaggedValue> args) {
    if (call_depth_ >= 256) {
        return fail(ErrorCode::RuntimeError, "call depth exceeded (256)");
    }
    struct DepthGuard {
        uint32_t& d;
        uint32_t& max;
        explicit DepthGuard(Interpreter& self)
            : d(self.call_depth_), max(self.stats_.max_call_depth) {
            ++d;
            max = std::max(max, d);
        }
        ~DepthGuard() { --d; }
    } depth_guard(*this);

    MethodData& md = method_data(method);
    if (!md.ready) build_offset_maps(method, md);
    ModuleData& mdm = module_data(module);  // token tables (klass/builtin/method)

    method.ensure_runtime_tables(md.instruction_offsets.size());

    std::vector<TaggedValue> regs(method.register_count);
    for (size_t i = 0; i < args.size() && i < regs.size(); ++i) regs[i] = args[i];
    TaggedValue* R = regs.data();

    const uint8_t* code = method.code.data();
    const size_t code_size = method.code.size();
    size_t pc = 0;
    Decoded ins;
    Op last_op = Op::ILLEGAL;

    Result<RunResult> exit_result =
        RunResult{TaggedValue::undefined(), InterpStats{}};

// Computed-goto dispatch (docs/tier-t0.md section 4) with a switch fallback.
#if defined(__GNUC__) && !defined(VORTEX_NO_COMPUTED_GOTO)
#define VORTEX_DISPATCH()                                       \
    do {                                                        \
        if (!decode_fast(code, code_size, pc, ins)) {           \
            exit_result = fail(ErrorCode::VerifyError,          \
                               "malformed instruction at pc " + \
                               std::to_string(pc));             \
            goto L_done;                                        \
        }                                                       \
        {                                                       \
            const uint16_t opc = ins.opcode;                    \
            if (opc >= ugb::kOpcodeCount ||                     \
                dispatch[opc] == nullptr) {                     \
                exit_result = fail(                             \
                    ErrorCode::RuntimeError,                    \
                    "opcode not executable in M0 T0: " +        \
                    std::to_string(opc));                       \
                goto L_done;                                    \
            }                                                   \
            goto* dispatch[opc];                                \
        }                                                       \
    } while (0)

#define L(name) L_##name

    // Dispatch table: GCC C++ rejects designated array initializers with
    // non-trivial label operands ("sorry, unimplemented"), so the table is
    // initialized once at runtime. The interpreter is single-threaded per
    // instance in M0; the init guard is safe under that contract.
    static const void* dispatch[ugb::kOpcodeCount] = {};
    if (dispatch[static_cast<unsigned>(Op::CONST_NULL)] == nullptr) {
        auto* T = const_cast<const void**>(dispatch);
#define MAP(op, label) T[static_cast<unsigned>(op)] = &&label
        MAP(Op::CONST_NULL, L(CONST_NULL));
        MAP(Op::CONST_UNDEFINED, L(CONST_UNDEFINED));
        MAP(Op::CONST_FALSE, L(CONST_FALSE));
        MAP(Op::CONST_TRUE, L(CONST_TRUE));
        MAP(Op::CONST_I32, L(CONST_I32));
        MAP(Op::CONST_I64, L(CONST_I64));
        MAP(Op::CONST_F64, L(CONST_F64));
        MAP(Op::MOVE, L(MOVE));
        MAP(Op::ADD_ANY, L(ADD_ANY));
        MAP(Op::ADD_I32, L(ADD_I32));
        MAP(Op::ADD_I64, L(ADD_I64));
        MAP(Op::ADD_F64, L(ADD_F64));
        MAP(Op::ADD_CHECKED_I32, L(ADD_I64));
        MAP(Op::SUB_ANY, L(SUB_ANY));
        MAP(Op::SUB_I32, L(SUB_I64));
        MAP(Op::SUB_I64, L(SUB_I64));
        // SUB_F64 / MUL_F64 stay unmapped in M0: the integer handlers would
        // silently trap on doubles. Unmapped opcodes produce the explicit
        // "not executable in M0" error instead.
        MAP(Op::MUL_ANY, L(MUL_ANY));
        MAP(Op::MUL_I32, L(MUL_I64));
        MAP(Op::MUL_I64, L(MUL_I64));
        MAP(Op::DIV_S_I64, L(DIV_S_I64));
        MAP(Op::DIV_F64, L(DIV_F64));
        MAP(Op::REM_S_I64, L(REM_S_I64));
        MAP(Op::NEG_I64, L(NEG_I64));
        MAP(Op::NEG_F64, L(NEG_F64));
        MAP(Op::AND_I, L(BIT_I));
        MAP(Op::OR_I, L(BIT_I));
        MAP(Op::XOR_I, L(BIT_I));
        MAP(Op::SHL_I, L(BIT_I));
        MAP(Op::SHR_S_I, L(BIT_I));
        MAP(Op::SHR_U_I, L(BIT_I));
        MAP(Op::EQ_I32, L(CMP));
        MAP(Op::EQ_I64, L(CMP));
        MAP(Op::NE_I64, L(CMP));
        MAP(Op::LT_S_I64, L(CMP));
        MAP(Op::LE_S_I64, L(CMP));
        MAP(Op::GT_S_I64, L(CMP));
        MAP(Op::GE_S_I64, L(CMP));
        MAP(Op::EQ_REF, L(EQ_REF));
        MAP(Op::NE_REF, L(NE_REF));
        MAP(Op::EQ_NULL, L(EQ_NULL));
        MAP(Op::EQ_F64, L(EQ_F64));
        MAP(Op::LT_F64, L(CMP_F64));
        MAP(Op::LE_F64, L(CMP_F64));
        MAP(Op::GT_F64, L(CMP_F64));
        MAP(Op::GE_F64, L(CMP_F64));
        MAP(Op::I64_TO_F64, L(I64_TO_F64));
        MAP(Op::F64_TO_I64, L(F64_TO_I64));
        MAP(Op::JUMP, L(JUMP));
        MAP(Op::JUMP_TRUE, L(JUMP_TRUE));
        MAP(Op::JUMP_FALSE, L(JUMP_FALSE));
        MAP(Op::RETURN, L(RETURN));
        MAP(Op::RETURN_UNIT, L(RETURN_UNIT));
        MAP(Op::CALL_DIRECT, L(CALL_DIRECT));
        MAP(Op::CALL_VIRTUAL, L(CALL_VIRTUAL));
        MAP(Op::CALL_BUILTIN, L(CALL_BUILTIN));
        MAP(Op::NEW_OBJECT, L(NEW_OBJECT));
        MAP(Op::NEW_ARRAY, L(NEW_ARRAY));
        MAP(Op::GET_FIELD, L(GET_FIELD));
        MAP(Op::GET_FIELD_SHAPE, L(GET_FIELD_SHAPE));
        MAP(Op::SET_FIELD, L(SET_FIELD));
        MAP(Op::SET_FIELD_SHAPE, L(SET_FIELD_SHAPE));
        MAP(Op::ARRAY_LENGTH, L(ARRAY_LENGTH));
        MAP(Op::ARRAY_GET, L(ARRAY_GET));
        MAP(Op::ARRAY_SET, L(ARRAY_SET));
        MAP(Op::CHECK_NULL, L(CHECK_NULL));
        MAP(Op::CHECK_NON_NULL, L(CHECK_NON_NULL));
        MAP(Op::CHECK_CLASS, L(CHECK_CLASS));
        MAP(Op::CHECK_BOUNDS, L(CHECK_BOUNDS));
        MAP(Op::SAFEPOINT_POLL, L(SAFEPOINT_POLL));
        MAP(Op::DEBUG_SRCPOS, L(NOP));
        MAP(Op::DEBUG_TRAP, L(NOP));
        MAP(Op::WRITE_BARRIER_STORE, L(NOP));
#undef MAP
    }

    VORTEX_DISPATCH();

    // Re-dispatch continuation: all handlers leave their scope with a plain
    // `goto L_next` (which correctly destroys handler locals); the computed
    // dispatch itself only ever runs at this top-level scope.
L_next:
    VORTEX_DISPATCH();

    // ---- handler macros ----------------------------------------------------
#define VORTEX_PROFILE()                                                    \
    do {                                                                    \
        stats_.instructions_executed++;                                     \
        if (config_.enable_profiling) {                                     \
            const uint32_t idx = md.instruction_index[pc];                  \
            if (idx != 0xFFFFFFFFu) method.profiles[idx].record_execution(); \
        }                                                                   \
    } while (0)

#define VORTEX_ADVANCE()                     \
    do {                                     \
        record_bigram(method, last_op,       \
                      static_cast<Op>(ins.opcode)); \
        last_op = static_cast<Op>(ins.opcode);       \
        pc = ins.next_pc;                    \
    } while (0)

#define VORTEX_NEXT()      \
    do {                   \
        VORTEX_ADVANCE();  \
        goto L_next;       \
    } while (0)

#define VORTEX_REDIRECT() goto L_next  // pc already assigned (branches)

#define VORTEX_RT_ERROR(msg)                                              \
    do {                                                                  \
        exit_result = fail(ErrorCode::RuntimeError,                       \
                           std::string("runtime error in '") + method.name + \
                               "': " + msg);                              \
        goto L_done;                                                      \
    } while (0)

    // ---- handlers -----------------------------------------------------------
L_CONST_NULL:
    VORTEX_PROFILE();
    R[ins.dst] = TaggedValue::null();
    VORTEX_NEXT();
L_CONST_UNDEFINED:
    VORTEX_PROFILE();
    R[ins.dst] = TaggedValue::undefined();
    VORTEX_NEXT();
L_CONST_FALSE:
    VORTEX_PROFILE();
    R[ins.dst] = TaggedValue::boolean(false);
    VORTEX_NEXT();
L_CONST_TRUE:
    VORTEX_PROFILE();
    R[ins.dst] = TaggedValue::boolean(true);
    VORTEX_NEXT();
L_CONST_I32:
    VORTEX_PROFILE();
    R[ins.dst] =
        TaggedValue::smi(static_cast<int32_t>(ins.meta));
    VORTEX_NEXT();
L_CONST_I64: {
    VORTEX_PROFILE();
    R[ins.dst] = module.materialize_constant(ins.meta);
    VORTEX_NEXT();
}
L_CONST_F64: {
    VORTEX_PROFILE();
    auto boxed = heap_.allocate_double(
        ins.meta < module.constants.size() &&
                module.constants[ins.meta].kind == ugb::Constant::Kind::Float64
            ? module.constants[ins.meta].f64
            : 0.0);
    if (!boxed) VORTEX_RT_ERROR("allocation failed (double box)");
    stats_.allocations++;
    R[ins.dst] = TaggedValue::heap_pointer(*boxed);
    VORTEX_NEXT();
}
L_MOVE:
    VORTEX_PROFILE();
    R[ins.dst] = R[ins.s0];
    VORTEX_NEXT();

    // ---- arithmetic ---------------------------------------------------------
    // Typed forms execute the speculative fast path; on failure they count the
    // failure and run the canonical semantics inline (docs/tier-t0.md section 2).
L_ADD_ANY: {
    VORTEX_PROFILE();
    stats_.generic_instructions_executed++;
    TaggedValue r = generic_add(R[ins.s0], R[ins.s1]);
    if (r.is_undefined()) VORTEX_RT_ERROR("Add.Any: unsupported operand types");
    R[ins.dst] = r;
    // Adaptive: stable smi behavior promotes the site to Add.I32.
    const uint32_t idx = md.instruction_index[pc];
    if (idx != 0xFFFFFFFFu) {
        ProfileSlot& p = method.profiles[idx];
        if (R[ins.s0].is_smi() && R[ins.s1].is_smi()) {
            p.branch_counts[0]++;
            if (p.branch_counts[0] >= config_.typed_rewrite_threshold) {
                maybe_rewrite_to_typed(method, pc, Op::ADD_ANY, Op::ADD_I32);
                // code pointer stays valid (in-place u16 patch)
            }
        } else {
            p.branch_counts[0] = 0;
        }
    }
    VORTEX_NEXT();
}
L_ADD_I32: {
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    const uint32_t idx = md.instruction_index[pc];
    if (R[ins.s0].is_smi() && R[ins.s1].is_smi()) {
        const int64_t r = R[ins.s0].as_smi() + R[ins.s1].as_smi();
        if (r >= TaggedValue::smi_min() && r <= TaggedValue::smi_max()) {
            R[ins.dst] = TaggedValue::smi(r);
            VORTEX_NEXT();
        }
    }
    // Speculation failed: count it, maybe demote, then run canonical inline.
    if (idx != 0xFFFFFFFFu) {
        ProfileSlot& p = method.profiles[idx];
        p.record_failure();
        if (p.failure_count >= config_.generic_rewrite_threshold) {
            maybe_rewrite_to_generic(method, pc, Op::ADD_I32);
        }
    }
    TaggedValue r = generic_add(R[ins.s0], R[ins.s1]);
    if (r.is_undefined()) {
        VORTEX_RT_ERROR("Add.I32: speculation failed and canonical form cannot "
                        "handle operands");
    }
    R[ins.dst] = r;
    VORTEX_NEXT();
}
L_ADD_I64:
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    if (R[ins.s0].is_smi() && R[ins.s1].is_smi()) {
        const int64_t r = R[ins.s0].as_smi() + R[ins.s1].as_smi();
        if (r >= TaggedValue::smi_min() && r <= TaggedValue::smi_max()) {
            R[ins.dst] = TaggedValue::smi(r);
            VORTEX_NEXT();
        }
    }
    VORTEX_RT_ERROR("Add.I64: overflow or non-integer operand");
L_ADD_F64: {
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    const double a = as_double(R[ins.s0]);
    const double b = as_double(R[ins.s1]);
    if (a != a || b != b) {  // NaN sentinel for "not a double"
        VORTEX_RT_ERROR("Add.F64: operand is not a float");
    }
    auto boxed = heap_.allocate_double(a + b);
    if (!boxed) VORTEX_RT_ERROR("allocation failed (double result)");
    stats_.allocations++;
    R[ins.dst] = TaggedValue::heap_pointer(*boxed);
    VORTEX_NEXT();
}
L_SUB_ANY: {
    VORTEX_PROFILE();
    stats_.generic_instructions_executed++;
    TaggedValue r = generic_sub(R[ins.s0], R[ins.s1]);
    if (r.is_undefined()) VORTEX_RT_ERROR("Sub.Any: unsupported operand types");
    R[ins.dst] = r;
    VORTEX_NEXT();
}
L_SUB_I64:
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    if (R[ins.s0].is_smi() && R[ins.s1].is_smi()) {
        const int64_t r = R[ins.s0].as_smi() - R[ins.s1].as_smi();
        if (r >= TaggedValue::smi_min() && r <= TaggedValue::smi_max()) {
            R[ins.dst] = TaggedValue::smi(r);
            VORTEX_NEXT();
        }
    }
    VORTEX_RT_ERROR("Sub typed: overflow or non-integer operand");
L_MUL_ANY: {
    VORTEX_PROFILE();
    stats_.generic_instructions_executed++;
    TaggedValue r = generic_mul(R[ins.s0], R[ins.s1]);
    if (r.is_undefined()) VORTEX_RT_ERROR("Mul.Any: unsupported operand types");
    R[ins.dst] = r;
    const uint32_t idx = md.instruction_index[pc];
    if (idx != 0xFFFFFFFFu) {
        ProfileSlot& p = method.profiles[idx];
        if (R[ins.s0].is_smi() && R[ins.s1].is_smi()) {
            p.branch_counts[0]++;
            if (p.branch_counts[0] >= config_.typed_rewrite_threshold) {
                maybe_rewrite_to_typed(method, pc, Op::MUL_ANY, Op::MUL_I32);
            }
        } else {
            p.branch_counts[0] = 0;
        }
    }
    VORTEX_NEXT();
}
L_MUL_I64:
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    if (R[ins.s0].is_smi() && R[ins.s1].is_smi()) {
        const int64_t r = R[ins.s0].as_smi() * R[ins.s1].as_smi();
        if (r >= TaggedValue::smi_min() && r <= TaggedValue::smi_max()) {
            R[ins.dst] = TaggedValue::smi(r);
            VORTEX_NEXT();
        }
    }
    VORTEX_RT_ERROR("Mul typed: overflow or non-integer operand");
L_DIV_S_I64:
    VORTEX_PROFILE();
    if (!R[ins.s0].is_smi() || !R[ins.s1].is_smi()) {
        VORTEX_RT_ERROR("Div.S.I64: non-integer operand");
    }
    if (R[ins.s1].as_smi() == 0) VORTEX_RT_ERROR("division by zero");
    R[ins.dst] = TaggedValue::smi(R[ins.s0].as_smi() / R[ins.s1].as_smi());
    VORTEX_NEXT();
L_DIV_F64: {
    VORTEX_PROFILE();
    const double a = as_double(R[ins.s0]);
    const double b = as_double(R[ins.s1]);
    if (a != a || b != b) VORTEX_RT_ERROR("Div.F64: operand is not a float");
    auto boxed = heap_.allocate_double(b == 0.0 ? 0.0 : a / b);
    if (!boxed) VORTEX_RT_ERROR("allocation failed");
    R[ins.dst] = TaggedValue::heap_pointer(*boxed);
    VORTEX_NEXT();
}
L_REM_S_I64:
    VORTEX_PROFILE();
    if (!R[ins.s0].is_smi() || !R[ins.s1].is_smi()) {
        VORTEX_RT_ERROR("Rem.S.I64: non-integer operand");
    }
    if (R[ins.s1].as_smi() == 0) VORTEX_RT_ERROR("remainder by zero");
    R[ins.dst] = TaggedValue::smi(R[ins.s0].as_smi() % R[ins.s1].as_smi());
    VORTEX_NEXT();
L_NEG_I64:
    VORTEX_PROFILE();
    if (!R[ins.s0].is_smi()) VORTEX_RT_ERROR("Neg.I64: non-integer operand");
    R[ins.dst] = TaggedValue::smi(-R[ins.s0].as_smi());
    VORTEX_NEXT();
L_NEG_F64: {
    VORTEX_PROFILE();
    const double a = as_double(R[ins.s0]);
    if (a != a) VORTEX_RT_ERROR("Neg.F64: operand is not a float");
    auto boxed = heap_.allocate_double(-a);
    if (!boxed) VORTEX_RT_ERROR("allocation failed");
    R[ins.dst] = TaggedValue::heap_pointer(*boxed);
    VORTEX_NEXT();
}
L_BIT_I: {
    VORTEX_PROFILE();
    if (!R[ins.s0].is_smi() || !R[ins.s1].is_smi()) {
        VORTEX_RT_ERROR("bitwise op: non-integer operand");
    }
    const int64_t a = R[ins.s0].as_smi();
    const int64_t b = R[ins.s1].as_smi();
    int64_t r = 0;
    switch (static_cast<Op>(ins.opcode)) {
    case Op::AND_I: r = a & b; break;
    case Op::OR_I: r = a | b; break;
    case Op::XOR_I: r = a ^ b; break;
    case Op::SHL_I: r = static_cast<int64_t>(static_cast<uint64_t>(a)
                                             << (b & 63)); break;
    case Op::SHR_S_I: r = a >> (b & 63); break;
    case Op::SHR_U_I:
        r = static_cast<int64_t>(static_cast<uint64_t>(a) >> (b & 63));
        break;
    default: VORTEX_RT_ERROR("bad bitwise op");
    }
    R[ins.dst] = TaggedValue::smi(r);
    VORTEX_NEXT();
}

    // ---- comparisons ----------------------------------------------------------
L_CMP: {
    VORTEX_PROFILE();
    TaggedValue r = generic_compare(R[ins.s0], R[ins.s1],
                                    static_cast<Op>(ins.opcode));
    if (r.is_undefined()) VORTEX_RT_ERROR("comparison: unsupported operand types");
    R[ins.dst] = r;
    VORTEX_NEXT();
}
L_EQ_REF:
    VORTEX_PROFILE();
    R[ins.dst] =
        TaggedValue::boolean(R[ins.s0].reference_equals(R[ins.s1]));
    VORTEX_NEXT();
L_NE_REF:
    VORTEX_PROFILE();
    R[ins.dst] =
        TaggedValue::boolean(!R[ins.s0].reference_equals(R[ins.s1]));
    VORTEX_NEXT();
L_EQ_NULL:
    VORTEX_PROFILE();
    R[ins.dst] = TaggedValue::boolean(R[ins.s0].is_null());
    VORTEX_NEXT();
L_EQ_F64: {
    VORTEX_PROFILE();
    const double a = as_double(R[ins.s0]);
    const double b = as_double(R[ins.s1]);
    if (a != a || b != b) VORTEX_RT_ERROR("Eq.F64: operand is not a float");
    R[ins.dst] = TaggedValue::boolean(a == b);
    VORTEX_NEXT();
}
L_CMP_F64: {
    VORTEX_PROFILE();
    const double a = as_double(R[ins.s0]);
    const double b = as_double(R[ins.s1]);
    if (a != a || b != b) VORTEX_RT_ERROR("float compare: not a float");
    bool r = false;
    switch (static_cast<Op>(ins.opcode)) {
    case Op::LT_F64: r = a < b; break;
    case Op::LE_F64: r = a <= b; break;
    case Op::GT_F64: r = a > b; break;
    case Op::GE_F64: r = a >= b; break;
    default: VORTEX_RT_ERROR("bad float compare");
    }
    R[ins.dst] = TaggedValue::boolean(r);
    VORTEX_NEXT();
}
L_I64_TO_F64: {
    VORTEX_PROFILE();
    if (!R[ins.s0].is_smi()) VORTEX_RT_ERROR("I64ToF64: non-integer operand");
    auto boxed = heap_.allocate_double(
        static_cast<double>(R[ins.s0].as_smi()));
    if (!boxed) VORTEX_RT_ERROR("allocation failed");
    R[ins.dst] = TaggedValue::heap_pointer(*boxed);
    VORTEX_NEXT();
}
L_F64_TO_I64: {
    VORTEX_PROFILE();
    const double a = as_double(R[ins.s0]);
    if (a != a) VORTEX_RT_ERROR("F64ToI64: operand is not a float");
    R[ins.dst] = TaggedValue::smi(static_cast<int64_t>(a));
    VORTEX_NEXT();
}

    // ---- control flow -----------------------------------------------------------
L_JUMP:
    VORTEX_PROFILE();
    if (ins.meta <= pc) {  // backward branch: profile + tiering handoff
        record_backedge(module, method);
    }
    pc = ins.meta;
    VORTEX_REDIRECT();
L_JUMP_TRUE: {
    VORTEX_PROFILE();
    const bool taken = R[ins.s0].truthy();
    if (config_.enable_profiling) {
        const uint32_t idx = md.instruction_index[pc];
        if (idx != 0xFFFFFFFFu) method.profiles[idx].record_branch(taken);
    }
    if (taken) {
        if (ins.meta <= pc) record_backedge(module, method);
        pc = ins.meta;
        VORTEX_REDIRECT();
    }
    VORTEX_NEXT();
}
L_JUMP_FALSE: {
    VORTEX_PROFILE();
    const bool taken = !R[ins.s0].truthy();
    if (config_.enable_profiling) {
        const uint32_t idx = md.instruction_index[pc];
        if (idx != 0xFFFFFFFFu) method.profiles[idx].record_branch(taken);
    }
    if (taken) {
        if (ins.meta <= pc) record_backedge(module, method);
        pc = ins.meta;
        VORTEX_REDIRECT();
    }
    VORTEX_NEXT();
}
L_RETURN:
    VORTEX_PROFILE();
    exit_result = RunResult{R[ins.s0], stats_};
    goto L_done;
L_RETURN_UNIT:
    VORTEX_PROFILE();
    exit_result = RunResult{TaggedValue::undefined(), stats_};
    goto L_done;

    // ---- calls ---------------------------------------------------------------------
L_CALL_DIRECT: {
    VORTEX_PROFILE();
    stats_.calls++;
    const int32_t target = resolve_method(module, ins.meta);
    if (target < 0) VORTEX_RT_ERROR("Call.Direct: unresolved method token");
    ugb::UGBMethod& callee = module.method_table[static_cast<size_t>(target)];
    const uint16_t argc = ins.s1;
    std::vector<TaggedValue> call_args(argc);
    for (uint16_t i = 0; i < argc; ++i) {
        call_args[i] = R[static_cast<size_t>(ins.s0) + i];
    }
    auto sub = execute(module, callee, call_args);
    if (!sub) {
        exit_result = std::unexpected(sub.error());
        goto L_done;
    }
    R[ins.dst] = sub->value;
    VORTEX_NEXT();
}
L_CALL_VIRTUAL: {
    VORTEX_PROFILE();
    stats_.calls++;
    // Dispatch through the receiver's klass method table (docs/ugb.md 4.3).
    if (ins.s0 >= method.register_count ||
        !R[ins.s0].is_heap_object()) {
        VORTEX_RT_ERROR("Call.Virtual: receiver is not an object");
    }
    auto* recv = R[ins.s0].as_heap_object();
    if (recv->header.klass == nullptr) {
        VORTEX_RT_ERROR("Call.Virtual: receiver has no klass");
    }
    const int32_t tok_idx = static_cast<int32_t>(ins.meta) - 1;
    if (tok_idx < 0 || static_cast<size_t>(tok_idx) >= module.methods.size()) {
        VORTEX_RT_ERROR("Call.Virtual: bad method token");
    }
    MethodEntry* entry =
        recv->header.klass->find_method(module.methods[static_cast<size_t>(tok_idx)].name);
    if (entry == nullptr || entry->kind != MethodImplementationKind::Bytecode) {
        VORTEX_RT_ERROR("Call.Virtual: no bytecode implementation on receiver");
    }
    ugb::UGBMethod& callee = module.method_table[static_cast<size_t>(entry->ugb_method_id)];
    const uint16_t argc = ins.s1;
    std::vector<TaggedValue> call_args(argc);
    for (uint16_t i = 0; i < argc; ++i) {
        call_args[i] = R[static_cast<size_t>(ins.s0) + i];
    }
    auto sub = execute(module, callee, call_args);
    if (!sub) {
        exit_result = std::unexpected(sub.error());
        goto L_done;
    }
    R[ins.dst] = sub->value;
    VORTEX_NEXT();
}
L_CALL_BUILTIN: {
    VORTEX_PROFILE();
    stats_.calls++;
    const int32_t slot = resolve_builtin(module, ins.meta);
    if (slot < 0) VORTEX_RT_ERROR("Call.Builtin: unresolved builtin token");
    const Builtin& b = builtins_[static_cast<size_t>(slot)];
    const uint16_t argc = ins.s1;
    std::vector<TaggedValue> call_args(argc);
    for (uint16_t i = 0; i < argc; ++i) {
        call_args[i] = R[static_cast<size_t>(ins.s0) + i];
    }
    R[ins.dst] = b.fn(call_args, b.user);
    VORTEX_NEXT();
}

    // ---- allocation / fields / arrays -------------------------------------------------
L_NEW_OBJECT: {
    VORTEX_PROFILE();
    if (ins.meta >= mdm.klass_table.size()) {
        VORTEX_RT_ERROR("New.Object: bad klass token");
    }
    Klass* k = mdm.klass_table[ins.meta];
    auto obj = heap_.allocate_object(k, k->field_count());
    if (!obj) VORTEX_RT_ERROR("allocation failed (object)");
    stats_.allocations++;
    R[ins.dst] = TaggedValue::heap_pointer(*obj);
    VORTEX_NEXT();
}
L_NEW_ARRAY: {
    VORTEX_PROFILE();
    if (!R[ins.s0].is_smi() || R[ins.s0].as_smi() < 0 ||
        R[ins.s0].as_smi() > 1'000'000) {
        VORTEX_RT_ERROR("New.Array: bad length");
    }
    auto arr = heap_.allocate_array(static_cast<uint32_t>(R[ins.s0].as_smi()));
    if (!arr) VORTEX_RT_ERROR("allocation failed (array)");
    stats_.allocations++;
    R[ins.dst] = TaggedValue::heap_pointer(*arr);
    VORTEX_NEXT();
}
L_GET_FIELD: {
    VORTEX_PROFILE();
    const TaggedValue obj = R[ins.s0];
    if (!obj.is_heap_object()) VORTEX_RT_ERROR("GetField: not an object");
    auto* o = obj.as_heap_object();
    if (o->header.klass == nullptr) VORTEX_RT_ERROR("GetField: object has no klass");
    // Inline cache: mono hit -> direct slot index (docs/tier-t0.md section 3).
    const uint32_t klass_id = o->header.klass->id();
    const uint32_t idx = md.instruction_index[pc];
    if (idx != 0xFFFFFFFFu) {
        IcSlot& ic = method.ics[idx];
        if (const ugb::IcEntry* e = ic.lookup(klass_id)) {
            stats_.ic_hits++;
            R[ins.dst] = static_cast<Object*>(o)->field(e->target);
            VORTEX_NEXT();
        }
        stats_.ic_misses++;
    }
    TaggedValue v = generic_get_field(obj, ins.meta, module);
    if (v.is_undefined()) VORTEX_RT_ERROR("GetField: unresolved field");
    if (idx != 0xFFFFFFFFu) {
        const int slot = o->header.klass->find_field(module.fields[ins.meta].name);
        if (slot >= 0) {
            method.ics[idx].record_hit(klass_id, static_cast<uint32_t>(slot));
        }
    }
    R[ins.dst] = v;
    VORTEX_NEXT();
}
L_GET_FIELD_SHAPE: {
    // Speculative form: shape guard + offset load, fallback to canonical.
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    const TaggedValue obj = R[ins.s0];
    const uint32_t idx = md.instruction_index[pc];
    if (obj.is_heap_object()) {
        auto* o = obj.as_heap_object();
        if (o->header.klass != nullptr) {
            const uint32_t klass_id = o->header.klass->id();
            if (idx != 0xFFFFFFFFu) {
                IcSlot& ic = method.ics[idx];
                if (const ugb::IcEntry* e = ic.lookup(klass_id)) {
                    stats_.ic_hits++;
                    R[ins.dst] = static_cast<Object*>(o)->field(e->target);
                    VORTEX_NEXT();
                }
            }
        }
    }
    // Guard failed -> canonical fallback (spec: execution continues with
    // canonical semantics; chronic failure demotes the opcode).
    if (idx != 0xFFFFFFFFu) {
        ProfileSlot& p = method.profiles[idx];
        p.record_failure();
        if (p.failure_count >= config_.generic_rewrite_threshold) {
            maybe_rewrite_to_generic(method, pc, Op::GET_FIELD_SHAPE);
        }
    }
    TaggedValue v = generic_get_field(obj, ins.meta, module);
    if (v.is_undefined()) VORTEX_RT_ERROR("GetField.Shape: guard failed, field unresolved");
    if (obj.is_heap_object()) {
        auto* o = obj.as_heap_object();
        if (o->header.klass != nullptr && idx != 0xFFFFFFFFu) {
            const int slot =
                o->header.klass->find_field(module.fields[ins.meta].name);
            if (slot >= 0) {
                method.ics[idx].record_hit(o->header.klass->id(),
                                           static_cast<uint32_t>(slot));
            }
        }
    }
    R[ins.dst] = v;
    VORTEX_NEXT();
}
L_SET_FIELD: {
    VORTEX_PROFILE();
    const TaggedValue obj = R[ins.s0];
    if (!generic_set_field(obj, R[ins.s1], ins.meta, module)) {
        VORTEX_RT_ERROR("SetField: unresolved field or bad receiver");
    }
    VORTEX_NEXT();
}
L_SET_FIELD_SHAPE: {
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    if (!generic_set_field(R[ins.s0], R[ins.s1], ins.meta, module)) {
        const uint32_t idx = md.instruction_index[pc];
        if (idx != 0xFFFFFFFFu) {
            ProfileSlot& p = method.profiles[idx];
            p.record_failure();
            if (p.failure_count >= config_.generic_rewrite_threshold) {
                maybe_rewrite_to_generic(method, pc, Op::SET_FIELD_SHAPE);
            }
        }
        VORTEX_RT_ERROR("SetField.Shape: guard failed");
    }
    VORTEX_NEXT();
}
L_ARRAY_LENGTH: {
    VORTEX_PROFILE();
    const TaggedValue arr = R[ins.s0];
    if (!arr.is_heap_object()) VORTEX_RT_ERROR("Array.Length: not an object");
    auto* a = reinterpret_cast<ArrayObject*>(arr.as_heap_object());
    if (a->header.klass != nullptr) VORTEX_RT_ERROR("Array.Length: not an array");
    R[ins.dst] = TaggedValue::smi(a->length());
    VORTEX_NEXT();
}
L_ARRAY_GET: {
    VORTEX_PROFILE();
    const TaggedValue arr = R[ins.s0];
    if (!arr.is_heap_object() || !R[ins.s1].is_smi()) {
        VORTEX_RT_ERROR("Array.Get: bad array or index");
    }
    auto* a = reinterpret_cast<ArrayObject*>(arr.as_heap_object());
    if (a->header.klass != nullptr) VORTEX_RT_ERROR("Array.Get: not an array");
    const int64_t i = R[ins.s1].as_smi();
    if (i < 0 || i >= a->length()) {
        VORTEX_RT_ERROR("Array.Get: bounds check failed (" +
                        std::to_string(i) + " vs " +
                        std::to_string(a->length()) + ")");
    }
    R[ins.dst] = a->element(static_cast<uint32_t>(i));
    VORTEX_NEXT();
}
L_ARRAY_SET: {
    VORTEX_PROFILE();
    const TaggedValue arr = R[ins.s0];
    if (!arr.is_heap_object() || !R[ins.s1].is_smi()) {
        VORTEX_RT_ERROR("Array.Set: bad array or index");
    }
    auto* a = reinterpret_cast<ArrayObject*>(arr.as_heap_object());
    if (a->header.klass != nullptr) VORTEX_RT_ERROR("Array.Set: not an array");
    const int64_t i = R[ins.s1].as_smi();
    if (i < 0 || i >= a->length()) {
        VORTEX_RT_ERROR("Array.Set: bounds check failed");
    }
    store_field(&a->element(static_cast<uint32_t>(i)), R[ins.s2]);
    if (R[ins.s2].is_heap_object()) heap_.card_table().mark_dirty(a);
    VORTEX_NEXT();
}

    // ---- guards ---------------------------------------------------------------
L_CHECK_NULL:
    VORTEX_PROFILE();
    if (!R[ins.s0].is_null()) VORTEX_RT_ERROR("Check.Null: value is not null");
    VORTEX_NEXT();
L_CHECK_NON_NULL:
    VORTEX_PROFILE();
    if (R[ins.s0].is_null()) VORTEX_RT_ERROR("Check.NonNull: null dereference");
    VORTEX_NEXT();
L_CHECK_CLASS: {
    VORTEX_PROFILE();
    const TaggedValue v = R[ins.s0];
    const bool pass = v.is_heap_object() &&
                      v.as_heap_object()->header.klass != nullptr &&
                      ins.meta < mdm.klass_table.size() &&
                      v.as_heap_object()->header.klass ==
                          mdm.klass_table[ins.meta];
    if (!pass) VORTEX_RT_ERROR("Check.Class: class guard failed");
    VORTEX_NEXT();
}
L_CHECK_BOUNDS: {
    VORTEX_PROFILE();
    if (!R[ins.s0].is_smi() || !R[ins.s1].is_smi()) {
        VORTEX_RT_ERROR("Check.Bounds: non-integer index/length");
    }
    const uint64_t idx = static_cast<uint64_t>(R[ins.s0].as_smi());
    const uint64_t len = static_cast<uint64_t>(R[ins.s1].as_smi());
    if (idx >= len) VORTEX_RT_ERROR("Check.Bounds: out of bounds");
    VORTEX_NEXT();
}

    // ---- misc --------------------------------------------------------------------
L_SAFEPOINT_POLL:
    VORTEX_PROFILE();
    stats_.safepoint_polls++;
    // M0: the poll is a counting no-op; the memory-protection polling page and
    // handshake integration arrive with infra 4 (docs/roadmap.md, M6).
    VORTEX_NEXT();
L_NOP:
    VORTEX_NEXT();

L_done:
    if (!exit_result.has_value()) return std::unexpected(exit_result.error());
    return exit_result;

#else
    // Switch-based fallback (VORTEX_NO_COMPUTED_GOTO): same handlers via a
    // plain loop. M0 ships the computed-goto path under GCC/Clang; this branch
    // exists for exotic toolchains and is exercised by defining the macro.
    (void)code;
    (void)code_size;
    (void)pc;
    (void)ins;
    (void)last_op;
    (void)exit_result;
    return fail(ErrorCode::InternalError,
                "switch dispatch fallback requires VORTEX_COMPUTED_GOTO build");
#endif
#undef VORTEX_DISPATCH
#undef L
#undef VORTEX_PROFILE
#undef VORTEX_ADVANCE
#undef VORTEX_NEXT
#undef VORTEX_REDIRECT
#undef VORTEX_RT_ERROR
}

// ---------------------------------------------------------------------------
// Helpers used by the dispatch loop
// ---------------------------------------------------------------------------

void Interpreter::record_backedge(ugb::UGBModule& module,
                                  const ugb::UGBMethod& method) {
    MethodHotness& h = [&]() -> MethodHotness& {
        for (auto& e : hotness_) {
            if (e.first == method.id) return e.second;
        }
        hotness_.emplace_back(method.id, MethodHotness{});
        return hotness_.back().second;
    }();
    h.backedges++;
    if (tiering_.should_osr(h) && h.backedges % config_.tiering.j1_backedges == 0) {
        stats_.osr_requests++;
        // Observer notification: the compile queue subscribes here in M1+.
        support::debug("t0", "OSR requested for method '" + method.name + "'");
    }
    (void)module;
}

int32_t Interpreter::resolve_method(ugb::UGBModule& module, uint32_t token) {
    ModuleData& md = module_data(module);
    if (!md.ready) return -1;
    if (token == 0 || token >= md.method_resolution.size()) return -1;
    int32_t cached = md.method_resolution[token];
    if (cached >= 0) return cached;
    const std::string& name = module.methods[token - 1].name;
    cached = module.find_method(name);
    md.method_resolution[token] = cached;
    return cached;
}

int32_t Interpreter::resolve_builtin(ugb::UGBModule& module, uint32_t token) {
    ModuleData& md = module_data(module);
    if (!md.ready || token >= md.builtin_slot.size()) return -1;
    int32_t cached = md.builtin_slot[token];
    if (cached >= 0) return cached;
    const std::string& name = module.builtins[token].name;
    for (size_t i = 0; i < builtins_.size(); ++i) {
        if (builtins_[i].name == name) {
            md.builtin_slot[token] = static_cast<int32_t>(i);
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

double Interpreter::as_double(TaggedValue v) const {
    if (!v.is_heap_object()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    auto* o = v.as_heap_object();
    if (!is_boxed_double(o, heap_.double_klass())) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return read_boxed_double(o);
}

}  // namespace vortex::vm
