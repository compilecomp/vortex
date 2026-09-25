#include "vortex/vm/interpreter.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <cstring>
#include <type_traits>

#include "vortex/support/cem.hpp"
#include "vortex/support/log.hpp"
#include "vortex/ugb/encoding.hpp"
#include "vortex/ugb/verifier.hpp"

// CEM-26 classification of this file (section 3):
//   @hot  — decode_fast, the dispatch loop in execute(), the generic_*
//           arithmetic slow paths, as_double, record_bigram, and the inline
//           key/index helpers below. Each carries a PERF_CONTRACT block.
//   @warm — get/set_field_cached, resolve_method/builtin, record_backedge,
//           the frame-pool growth path.
//   @cold — run() boundary, capability negotiation, module runtime build,
//           offset-map build, adaptive rewrites, transition recording,
//           builtin_print, all diagnostics.
namespace vortex::vm {

using vortex::support::ErrorCode;
using vortex::support::fail;
using vortex::support::ok;
using vortex::support::Result;
using ugb::IcSlot;
using ugb::Op;
using ugb::ProfileSlot;

namespace {

using ugb::encoding::kDstBytes;
using ugb::encoding::kFastDecodeMaxSrcs;
using ugb::encoding::kFlagsBytes;
using ugb::encoding::kInstrHeaderBytes;
using ugb::encoding::kMetaBytes;
using ugb::encoding::kOpcodeBytes;
using ugb::encoding::kSrcSlotBytes;

/// Sentinel for "no profile/IC site at this pc" (Rule 72: named, not magic).
constexpr uint32_t kInvalidInstructionIndex = 0xFFFFFFFFu;

/// Sentinel for "slot unresolved" — returned by the field-cache fills and
/// the token resolvers (CEM-26 section 2: named, not a bare literal).
constexpr int32_t kNoSlot = -1;

// @hot
// PERF_CONTRACT:
// BUDGET: <= 1 cycle (shift-or fold + OR; single u64 key materializes)
// READS: 0 (pure register arithmetic)
// WRITES: 0
// BRANCHES: 0
// CACHE: n/a (no memory)
//
// PERF_OBSERVATION:
// TARGET: Zen 5 / ARM Neoverse V2
// VALIDATED: g++ 14.2, -O2 -fno-rtti
// ACTUAL: 1 shr + 1 or, zero memory ops (compiler explorer, -O2)
// LAST_VALIDATED: 2026-09-22
inline uint64_t klass_token_key(uint32_t klass_id, uint32_t token) noexcept {
    constexpr uint32_t kIdShiftBits = 32;
    return (static_cast<uint64_t>(klass_id) << kIdShiftBits) | token;
}

// @hot
// PERF_CONTRACT:
// BUDGET: <= 4 cycles (one guarded load; branch fully predicted after warmup)
// READS: 4 bytes (one u32 from the pc->index map, same line as neighbors)
// WRITES: 0
// BRANCHES: 1 bounds test, predictable (pc is in-bounds in verified code)
// CACHE: the instruction_index vector line for pc
//
// PERF_OBSERVATION:
// TARGET: Zen 5 / ARM Neoverse V2
// VALIDATED: g++ 14.2, -O2
// ACTUAL: inlined at all call sites; 1 cmp + 1 cmov, load hoisted by the
//         decoder's dominant access pattern (map line shared with pc's code)
// LAST_VALIDATED: 2026-09-22
inline uint32_t instruction_index_at(const ugb::UGBMethod& m, size_t pc) noexcept {
    const std::vector<uint32_t>& v = m.runtime.instruction_index;
    return pc < v.size() ? v[pc] : kInvalidInstructionIndex;
}

// ---------------------------------------------------------------------------
// Compact decoded instruction kept in registers inside the loop.
//
// Layout note (CEM-26 section 9): AoS is correct here — every handler reads
// opcode + operands + meta of the SAME instruction together, so splitting
// into SoA would add one load per field per instruction. Field order is
// chosen so the 8-byte pointer-sized next_pc leads (no tail padding) and
// meta sits beside the opcode lane that branch/call handlers read together:
// the struct packs to exactly 24 bytes = three register lanes, with zero
// internal padding.
struct Decoded {
    size_t next_pc = 0;     // lane 3: branch target / continuation pc
    uint32_t meta = 0;      // lane 2a: token / immediate / absolute target
    uint16_t opcode = 0;    // lane 2b
    uint16_t dst = 0;       // lane 1a
    uint16_t s0 = 0;        // lane 1b
    uint16_t s1 = 0;        // lane 1c
    uint16_t s2 = 0;        // lane 1d
    uint8_t nsrc = 0;       // tail: source count
    bool has_meta = false;  // tail: meta-presence flag
};

static_assert(sizeof(Decoded) == 24,
              "Decoded is carried across dispatch jumps in registers; growth "
              "here is a per-instruction cost event, not a free change");
static_assert(alignof(Decoded) == 8, "Decoded is word-addressed");
static_assert(std::is_trivially_copyable_v<Decoded>,
              "Decoded must stay trivially copyable (CEM-26 section 8)");

// @hot
// PERF_CONTRACT:
// BUDGET: <= 7 cycles/instruction (2 overlapping u64 loads + shift-or folds)
// READS: 6-byte header + 2*src_count + optional 4-byte meta, all sequential
// WRITES: 24 bytes into the register-carried Decoded (no store until exit)
// BRANCHES: 2 (header bounds test, meta-presence test) + 1 (wide-src test,
//           not-taken in verified hot code)
// CACHE: one code-stream line per ~10 instructions; no pointer chasing
//
// PERF_OBSERVATION:
// TARGET: Zen 5 / ARM Neoverse V2
// VALIDATED: g++ 14.2, -O2 -fno-rtti
// ACTUAL: PENDING microbench (M0); source cost above is the contract of
//         record — see docs/cem26.md section 4 validation plan
// LAST_VALIDATED: 2026-09-22
inline bool decode_fast(const uint8_t* code, size_t size, size_t pc,
                        Decoded& d) noexcept {
    constexpr size_t kLane1 = 1 * ugb::encoding::kBitsPerByte;   // u16 hi
    constexpr size_t kLane2 = 2 * ugb::encoding::kBitsPerByte;   // u32 b2
    constexpr size_t kLane3 = 3 * ugb::encoding::kBitsPerByte;   // u32 b3
    if (pc + kInstrHeaderBytes > size) return false;
    d.opcode = static_cast<uint16_t>(
        code[pc] | (code[pc + 1] << kLane1));  // little-endian wire order
    const size_t flags_pos = pc + kOpcodeBytes;
    const uint8_t flags = code[flags_pos];
    const size_t dst_pos = flags_pos + kFlagsBytes;
    d.dst = static_cast<uint16_t>(code[dst_pos] | (code[dst_pos + 1] << kLane1));
    const size_t nsrc_pos = dst_pos + kDstBytes;
    d.nsrc = code[nsrc_pos];
    size_t p = pc + kInstrHeaderBytes;
    if (d.nsrc > kFastDecodeMaxSrcs) {
        // Wide forms (>3 srcs) exist in the ISA; decode lazily — the
        // register-window handlers reject them at the boundary.
        if (p + static_cast<size_t>(d.nsrc) * kSrcSlotBytes > size) return false;
        p += static_cast<size_t>(d.nsrc) * kSrcSlotBytes;
        d.s0 = d.s1 = d.s2 = 0;
    } else {
        auto rd = [&](size_t i) -> uint16_t {
            const size_t lo = p + i * kSrcSlotBytes;
            return lo + 1 < size
                       ? static_cast<uint16_t>(code[lo] |
                                               (code[lo + 1] << kLane1))
                       : 0;
        };
        d.s0 = d.nsrc > 0 ? rd(0) : 0;
        d.s1 = d.nsrc > 1 ? rd(1) : 0;
        d.s2 = d.nsrc > 2 ? rd(2) : 0;
        p += static_cast<size_t>(d.nsrc) * kSrcSlotBytes;
    }
    d.has_meta = (flags & ugb::FLAG_HAS_META) != 0;
    if (d.has_meta) {
        if (p + kMetaBytes > size) return false;
        d.meta = static_cast<uint32_t>(code[p]) |
                 (static_cast<uint32_t>(code[p + 1]) << kLane1) |
                 (static_cast<uint32_t>(code[p + 2]) << kLane2) |
                 (static_cast<uint32_t>(code[p + 3]) << kLane3);
        p += kMetaBytes;
    }
    d.next_pc = p;
    return true;
}

// @cold
// Diagnostic sink only: builtin call sites are guest-visible operations, so
// the print implementation may build strings and hit stdio (CEM-26 section
// 3). Not reachable from the dispatch loop except through the CALL_BUILTIN
// handler, whose contract covers the boundary.
TaggedValue builtin_print(std::span<const TaggedValue> args, void*) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) std::fputs(" ", stdout);
        const std::string s = args[i].to_string();
        std::fputs(s.c_str(), stdout);
    }
    std::fputs("\n", stdout);
    return TaggedValue::undefined();
}

// The T0 engine never mutates builtin registration behind the host's back;
// hosts (vx, tests) register their own print implementation explicitly.
[[maybe_unused]] TaggedValue (*const kDefaultPrint)(
    std::span<const TaggedValue>, void*) = builtin_print;

}  // namespace

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
// Per-module runtime build (klass handles + token caches). @cold — runs once
// per module per engine instance (module-runtime ready flag gates it).
// ---------------------------------------------------------------------------

Result<void> Interpreter::build_module_runtime(ugb::UGBModule& module) {
    ugb::ModuleRuntimeData& rt = module.runtime;
    if (rt.ready) return ok();

    // Class tokens -> klass handles. Klass layouts are built from the
    // module's DECLARED field tokens (owner class known from "Class.field"
    // refs) so NEW_OBJECT allocates the full field array up front. Tokens
    // created by bare instruction references (declared=false, UGB v3) never
    // shape a layout: access to one resolves through find_field -> miss ->
    // the canonical unresolved-field error instead of an out-of-bounds slot.
    rt.klass_table.clear();
    for (const auto& c : module.classes) {
        rt.klass_table.push_back(registry_.create(c.name));
    }
    for (const auto& f : module.fields) {
        if (f.declared && f.klass_token < rt.klass_table.size()) {
            static_cast<Klass*>(rt.klass_table[f.klass_token])->add_field(f.name);
        }
    }
    rt.field_slot.assign(module.fields.size(), -1);
    rt.builtin_slot.assign(module.builtins.size(), -1);
    rt.method_resolution.assign(module.methods.size() + 1, -1);
    rt.ready = true;
    return ok();
}

// @cold — once per method per engine instance (offsets_ready gate).
void build_offset_maps(ugb::UGBMethod& m) {
    ugb::InstructionStream stream(m.code.data(), m.code.size());
    m.runtime.instruction_offsets.clear();
    size_t off = 0;
    while (off < m.code.size()) {
        m.runtime.instruction_offsets.push_back(static_cast<uint32_t>(off));
        ugb::Instruction instr;
        if (!stream.decode_at(off, instr)) break;
    }
    m.runtime.instruction_index.assign(m.code.size(), kInvalidInstructionIndex);
    const size_t n = m.runtime.instruction_offsets.size();
    for (size_t i = 0; i < n; ++i) {
        const uint32_t start = m.runtime.instruction_offsets[i];
        const uint32_t end = i + 1 < n
                                 ? m.runtime.instruction_offsets[i + 1]
                                 : static_cast<uint32_t>(m.code.size());
        for (uint32_t b = start; b < end; ++b) {
            m.runtime.instruction_index[b] = static_cast<uint32_t>(i);
        }
    }
    m.runtime.offsets_ready = true;
}

// ---------------------------------------------------------------------------
// Capability negotiation (Rule 3): unknown or unsupported capabilities cause
// safe rejection with a diagnostic — never silent misexecution. @cold.
// ---------------------------------------------------------------------------

Result<void> Interpreter::check_capabilities(const ugb::UGBModule& module) const {
    const ugb::CapabilitySet advertised = ugb::engine_advertised_capabilities();
    for (const ugb::UGBMethod& m : module.method_table) {
        for (uint8_t raw : m.required_capabilities) {
            if (!ugb::is_valid_capability(raw)) {
                return fail(ErrorCode::VerifyError,
                            "method '" + m.name + "' requires unknown "
                            "capability id " + std::to_string(raw) +
                                " (safe rejection, Rule 3)");
            }
            if (!advertised.supports_raw(raw)) {
                return fail(
                    ErrorCode::VerifyError,
                    "method '" + m.name + "' requires capability '" +
                        ugb::capability_name(static_cast<ugb::Capability>(raw)) +
                        "' which this engine does not support (safe "
                        "rejection, Rule 3)");
            }
        }
    }
    return ok();
}

// ---------------------------------------------------------------------------
// Generic (canonical) slow paths — the ADD_ANY-style fallbacks the spec
// describes as "handle all cases but are slower". Numeric semantics are
// exact (Rule 110): overflow is a distinct defined trap, never a wrapped or
// truncated value; type errors are distinct from overflow.
//
// All four are @hot: they execute on every canonical-arithmetic instruction
// and on every speculation failure of the typed forms. They must stay small
// enough for inlining into their dispatch handlers (CEM-26 section 11).
// ---------------------------------------------------------------------------

// @hot
// PERF_CONTRACT:
// BUDGET: <= 6 cycles for the smi-smi case (2 tag tests + 1 add + range check
//         folded into the overflow builtin's flags)
// READS: 16 bytes (two tagged words, already in registers at the handler)
// WRITES: 0 (16-byte ArithResult returned in registers)
// BRANCHES: 2 (both tag tests, predicted taken in smi-typed loops; the
//           overflow test is one __builtin_add_overflow branch)
// CACHE: none beyond the register file
//
// PERF_OBSERVATION:
// TARGET: Zen 5 / ARM Neoverse V2
// VALIDATED: g++ 14.2, -O2 -fno-rtti
// ACTUAL: PENDING microbench (M0); smi path emits lea + 2 test/jcc + jo —
//         see docs/cem26.md section 4 validation plan
// LAST_VALIDATED: 2026-09-22
Interpreter::ArithResult Interpreter::generic_add(TaggedValue a,
                                                  TaggedValue b) const noexcept {
    if (a.is_smi() && b.is_smi()) {
        int64_t r = 0;
        if (__builtin_add_overflow(a.as_smi(), b.as_smi(), &r) ||
            r < TaggedValue::smi_min() || r > TaggedValue::smi_max()) {
            return {TaggedValue::undefined(), ArithStatus::SmiOverflow};
        }
        return {TaggedValue::smi(r), ArithStatus::Ok};
    }
    return {TaggedValue::undefined(), ArithStatus::TypeError};
}

// @hot — see generic_add for the shared cost block; sub differs only in the
// overflow builtin (1 sub instead of 1 add, same register budget).
// PERF_CONTRACT: see generic_add (shared block of record).
Interpreter::ArithResult Interpreter::generic_sub(TaggedValue a,
                                                  TaggedValue b) const noexcept {
    if (a.is_smi() && b.is_smi()) {
        int64_t r = 0;
        if (__builtin_sub_overflow(a.as_smi(), b.as_smi(), &r) ||
            r < TaggedValue::smi_min() || r > TaggedValue::smi_max()) {
            return {TaggedValue::undefined(), ArithStatus::SmiOverflow};
        }
        return {TaggedValue::smi(r), ArithStatus::Ok};
    }
    return {TaggedValue::undefined(), ArithStatus::TypeError};
}

// @hot — see generic_add; mul differs only in the overflow builtin (1 imul,
// 3-cycle latency, still within the 6-cycle budget).
// PERF_CONTRACT: see generic_add (shared block of record).
Interpreter::ArithResult Interpreter::generic_mul(TaggedValue a,
                                                  TaggedValue b) const noexcept {
    if (a.is_smi() && b.is_smi()) {
        int64_t r = 0;
        if (__builtin_mul_overflow(a.as_smi(), b.as_smi(), &r) ||
            r < TaggedValue::smi_min() || r > TaggedValue::smi_max()) {
            return {TaggedValue::undefined(), ArithStatus::SmiOverflow};
        }
        return {TaggedValue::smi(r), ArithStatus::Ok};
    }
    return {TaggedValue::undefined(), ArithStatus::TypeError};
}

// @hot
// PERF_CONTRACT:
// BUDGET: <= 7 cycles (2 tag tests + compare + boolean tag store; the switch
//         folds to one setcc after constant-folding of `cmp` at inlined
//         call sites)
// READS: 16 bytes (two tagged words, register-resident)
// WRITES: 0 (16-byte result in registers)
// BRANCHES: 2 tag tests + the comparison's setcc (branchless result)
// CACHE: none beyond the register file
//
// PERF_OBSERVATION:
// TARGET: Zen 5 / ARM Neoverse V2
// VALIDATED: g++ 14.2, -O2 -fno-rtti
// ACTUAL: PENDING microbench (M0); inlined call sites emit one setcc per
//         comparison — see docs/cem26.md section 4
// LAST_VALIDATED: 2026-09-22
Interpreter::ArithResult Interpreter::generic_compare(TaggedValue a,
                                                      TaggedValue b,
                                                      Op cmp) const noexcept {
    if (!a.is_smi() || !b.is_smi()) {
        return {TaggedValue::undefined(), ArithStatus::TypeError};
    }
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
    default: return {TaggedValue::undefined(), ArithStatus::TypeError};
    }
    return {TaggedValue::boolean(r), ArithStatus::Ok};
}

// ---------------------------------------------------------------------------
// Field access: IC first, then a (klass_id, field_token) cache; the string
// lookup is the cold path that fills the cache (Rule 5).
// ---------------------------------------------------------------------------

// @warm — the IC fast path in GET_FIELD handlers absorbs mono/poly hits;
// this cache is the second layer (first call per (klass, field) pair, then
// pure flat-map hits).
// PERF_CONTRACT:
// BUDGET: <= 25 cycles warm (one hash fold + ~1 probe + slot load); first
//         touch per pair pays the cold string-compare fill (documented,
//         bounded by field-table size)
// READS: 64 (hash line) + 8 (slot)
// WRITES: 0 warm / insert on fill
// BRANCHES: 3 (receiver tag, cache probe, resolve)
// CACHE: FlatHashMap slot line; object header line
// PERF_NOTE: the cold fill is reachable from warm code by design — it is
//         the boundary that keeps string compares off the hot path (Rule 5).
bool Interpreter::get_field_cached(TaggedValue obj, uint32_t field_token,
                                   ugb::UGBModule& module, TaggedValue& out,
                                   int32_t* resolved_slot) {
    if (resolved_slot != nullptr) *resolved_slot = kNoSlot;
    if (!obj.is_heap_object() || field_token >= module.fields.size()) {
        return false;
    }
    auto* o = obj.as_heap_object();
    auto* k = o->header.klass;
    if (k == nullptr) return false;
    auto* objp = static_cast<Object*>(o);
    const uint64_t key = klass_token_key(k->id(), field_token);
    if (int32_t* slot = module.runtime.field_slot_cache.find(key)) {
        if (resolved_slot != nullptr) *resolved_slot = *slot;
        out = objp->field(static_cast<uint32_t>(*slot));
        return true;
    }
    const int idx = k->find_field(module.fields[field_token].name);
    if (idx < 0) return false;
    module.runtime.field_slot_cache.insert(key, idx);
    if (resolved_slot != nullptr) *resolved_slot = idx;
    out = objp->field(static_cast<uint32_t>(idx));
    return true;
}

// @warm — same two-layer scheme as get_field_cached; the card-mark is an
// unconditional 1-store on every reference-valued set (GC contract,
// docs/tier-t0.md section 8).
// PERF_CONTRACT:
// BUDGET: <= 30 cycles warm (probe + slot store + card store)
// READS: 64 (hash line) + 8 (slot)
// WRITES: 8 (field) + 1 (card byte, may hit the same line repeatedly)
// BRANCHES: 3 (receiver tag, probe, reference test for the card mark)
// CACHE: hash line, field line, card-table line (512-byte card granularity
//        keeps the card byte line-stable across neighboring stores)
bool Interpreter::set_field_cached(TaggedValue obj, TaggedValue value,
                                   uint32_t field_token,
                                   ugb::UGBModule& module) {
    if (!obj.is_heap_object() || field_token >= module.fields.size()) return false;
    auto* o = obj.as_heap_object();
    auto* k = o->header.klass;
    if (k == nullptr) return false;
    auto* objp = static_cast<Object*>(o);
    const uint64_t key = klass_token_key(k->id(), field_token);
    if (int32_t* slot = module.runtime.field_slot_cache.find(key)) {
        store_field(&objp->field(static_cast<uint32_t>(*slot)), value);
        if (value.is_heap_object()) heap_.card_table().mark_dirty(o);
        return true;
    }
    const int idx = k->find_field(module.fields[field_token].name);
    if (idx < 0) return false;
    module.runtime.field_slot_cache.insert(key, idx);
    store_field(&objp->field(static_cast<uint32_t>(idx)), value);
    if (value.is_heap_object()) heap_.card_table().mark_dirty(o);
    return true;
}

// ---------------------------------------------------------------------------
// Adaptive rewriting (docs/tier-t0.md section 6). @cold: both functions fire
// only when a named threshold crosses (typed_rewrite_threshold /
// generic_rewrite_threshold); the per-instruction cost is the two counter
// tests at the call site, covered by the calling handler's contract. The
// patch itself is a 2-byte in-place opcode swap — the instruction stream is
// method-owned and single-mutator (no fences needed; cross-thread IC patch
// publication arrives with J1/J2, Rule 89).
// ---------------------------------------------------------------------------

void Interpreter::maybe_rewrite_to_typed(ugb::UGBMethod& m, size_t pc,
                                         Op canonical, Op typed) {
    if (!config_.enable_adaptive_rewriting) return;
    uint8_t* raw = m.code.data() + pc;
    const uint16_t typed_bits = static_cast<uint16_t>(typed);
    raw[0] = static_cast<uint8_t>(typed_bits & 0xFF);
    raw[1] = static_cast<uint8_t>(typed_bits >> 8);
    stats_.typed_rewrites++;
    support::debug("t0", "rewrote " + std::string(ugb::opcode_name(canonical)) +
                             " -> " + std::string(ugb::opcode_name(typed)) +
                             " in method '" + m.name + "'");
}

void Interpreter::maybe_rewrite_to_generic(ugb::UGBMethod& m, size_t pc,
                                           Op typed) {
    if (!config_.enable_adaptive_rewriting) return;
    const Op canonical = ugb::canonical_form(typed);
    if (canonical == typed) return;
    uint8_t* raw = m.code.data() + pc;
    const uint16_t canon_bits = static_cast<uint16_t>(canonical);
    raw[0] = static_cast<uint8_t>(canon_bits & 0xFF);
    raw[1] = static_cast<uint8_t>(canon_bits >> 8);
    stats_.generic_rewrites++;
    support::debug("t0", "demoted " + std::string(ugb::opcode_name(typed)) +
                             " -> " + std::string(ugb::opcode_name(canonical)) +
                             " in method '" + m.name + "'");
}

// @hot — runs on EVERY dispatched instruction via VORTEX_ADVANCE.
// PERF_CONTRACT:
// BUDGET: <= 12 cycles (splitmix64 finalizer + ~1 linear probe + 1 increment)
// READS: 64 bytes (one FlatHashMap slot line, line-stable while hot)
// WRITES: 8 bytes on the same line (counter increment)
// BRANCHES: 1 probe comparison (empty-terminates), well predicted once the
//           working set of active bigrams is resident
// CACHE: one slot line; the hot bigram working set fits L1 by construction
//
// PERF_OBSERVATION:
// TARGET: Zen 5 / ARM Neoverse V2
// VALIDATED: g++ 14.2, -O2 -fno-rtti
// ACTUAL: PENDING microbench (M0) — see docs/cem26.md section 4
// LAST_VALIDATED: 2026-09-22
//
// PERF_PERMIT PERF-004:
// REASON: bigram heat is the J1 superstencil promotion input required by
//         docs/tier-j1.md section 3 — it cannot be sampled without losing
//         the tail of the distribution that drives fusion decisions.
// COST: one open-addressing probe per instruction (~12 cycles of the
//       dispatch budget); total loop cost stays within the execute()
//       contract with the probe included.
// ALLOCATION BOUNDARY: a new bigram key pays one FlatHashMap insert; the
//       key space is bounded by the opcode alphabet (kOpcodeCount squared),
//       so the table reaches steady state and stops allocating after
//       warmup. Steady-state execution performs zero allocation.
// OWNER: @vortex/rt (registered in docs/cem26.md section 5)
void Interpreter::record_bigram(Op first, Op second) {
    // Bigram hotness feeds superstencil promotion in J1 (docs/tier-j1.md
    // section 3). Counted into the module-level flat table.
    constexpr uint32_t kBigramOpcodeShiftBits = 16;  // Op is u16: low half
    const uint64_t key =
        (static_cast<uint64_t>(first) << kBigramOpcodeShiftBits) |
        static_cast<uint64_t>(second);
    if (uint64_t* c = bigram_counts_.find(key)) {
        ++*c;
    } else {
        bigram_counts_.insert(key, 1);
    }
}

// @cold — transition records are edge-triggered events (Rule 28); the shift
// only runs once the bounded buffer is full.
void Interpreter::record_transition(uint32_t method_id, uint32_t pc, Tier from,
                                    Tier to, TierTransitionRecord::Kind kind,
                                    const char* reason) {
    // Transitions are rare events; a shifting buffer keeps the exposed span
    // in chronological order (a wrap-around ring would need reordering to
    // present, which the span API cannot do).
    if (transitions_size_ < transitions_.size()) {
        transitions_[transitions_size_++] =
            TierTransitionRecord{method_id, pc, from, to, kind, reason};
        return;
    }
    std::memmove(transitions_.data(), transitions_.data() + 1,
                 (transitions_.size() - 1) * sizeof(TierTransitionRecord));
    transitions_.back() =
        TierTransitionRecord{method_id, pc, from, to, kind, reason};
}

// ---------------------------------------------------------------------------
// Entry points. @cold boundary: run() validates, negotiates and resolves
// once, then hands off to the @hot execute() loop. String work (entry name,
// diagnostics) is confined here so the hot loop never touches std::string.
// ---------------------------------------------------------------------------

Result<RunResult> Interpreter::run(ugb::UGBModule& module, std::string_view entry,
                                   std::span<const TaggedValue> args) {
    const int32_t mid = module.find_method(std::string(entry));
    if (mid < 0) {
        return fail(ErrorCode::RuntimeError,
                    "entry method '" + std::string(entry) + "' not found");
    }
    ugb::UGBMethod& method = module.method_table[static_cast<size_t>(mid)];

    // Mandatory verification before execution (Rule 7). The artifact is
    // untrusted (Rule 9); verification is deterministic, so a module whose
    // bytecode is unchanged reuses the verdict on later run() calls.
    if (!module.runtime.ready) {
        auto verified = ugb::verify_module(module);
        if (!verified) return std::unexpected(verified.error());

        // Rule 3: negotiate capabilities before any execution.
        auto caps = check_capabilities(module);
        if (!caps) return std::unexpected(caps.error());

        auto built = build_module_runtime(module);
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

// @hot — the T0 engine core: this function IS the interpreter.
// PERF_CONTRACT (master; per-handler budgets live at the handlers):
// BUDGET: <= 20 cycles/instruction steady-state = decode (7) + dispatch
//         jump (~3, BTB-predicted after warmup) + handler body (2-8) +
//         bigram probe (12, permitted — see record_bigram). Handlers that
//         allocate or call out carry their own contract at the site.
// READS: 6-byte header + operands per instruction; register file (R) is a
//        pinned contiguous buffer — no per-instruction reload of frame state
// WRITES: 8 bytes per executed handler (result register) + stats counters
//         (L1-resident)
// BRANCHES: 1 decode-bounds test, 1 dispatch jump (indirect, threaded — see
//           PERF_PERMIT below), handler-local tests as documented
// CACHE: code-stream line shared with the decode window; register-file line
//        per live window; bigram slot line. No pointer chasing anywhere in
//        the loop.
// ALLOCATION: zero in steady state — frame pool reuse (Rule 67); growth
//        paths are reachable only on new max depth/register count and are
//        documented at the growth site below. Third warm allocation: the
//        bigram table rehash (see record_bigram — bounded by the opcode
//        alphabet, stops after warmup).
//
// PERF_OBSERVATION:
// TARGET: Zen 5 / ARM Neoverse V2
// VALIDATED: g++ 14.2, -O2 -fno-rtti
// ACTUAL: PENDING microbench (M0); the source-cost budget above is the
//         review baseline until the harness lands — see docs/cem26.md
//         section 4 validation plan
// LAST_VALIDATED: 2026-09-22
//
// PERF_PERMIT PERF-001 (computed-goto dispatch):
// REASON: CEM-26 section 10 bans indirect CALLS in inner loops; the
//         dispatch here is an indirect JUMP within this same function —
//         threaded dispatch (ADR-002). It is the data-oriented dispatch the
//         standard's own vocabulary prescribes: one BTB-predicted branch,
//         no stack traffic, no call/ret pair, handler bodies inline-sized.
// COST: one indirect jump per instruction; alternation of hot opcodes is
//       handled by the BTB, mispredict cost is bounded by handler size.
// OWNER: @vortex/rt (ADR-002, docs/cem26.md section 5)
//
// PERF_NOTE (memory orders): the dispatch table publication below uses
// acquire/release only. seq_cst is deliberately avoided (CEM-26 section
// 14): the table is immutable once published and the single-writer
// protocol (ADR-002) needs no total order.
Result<RunResult> Interpreter::execute(ugb::UGBModule& module,
                                       ugb::UGBMethod& method,
                                       std::span<const TaggedValue> args) {
    return execute_impl(module, method, args, 0, 0xFFFFFFFFu,
                        TaggedValue::undefined());
}

Result<TaggedValue> Interpreter::resume(ugb::UGBModule& module,
                                        uint32_t method_id,
                                        std::span<const TaggedValue> vregs,
                                        uint32_t pc, uint32_t inject_dst,
                                        TaggedValue inject) {
    if (method_id >= module.method_table.size()) {
        return fail(ErrorCode::InvalidArgument, "resume: no such method");
    }
    auto& method = module.method_table[method_id];
    auto rr = execute_impl(module, method, vregs, pc, inject_dst, inject);
    if (!rr) return std::unexpected(std::move(rr).error());
    return rr->value;
}

Result<RunResult> Interpreter::execute_impl(ugb::UGBModule& module,
                                            ugb::UGBMethod& method,
                                            std::span<const TaggedValue> args,
                                            size_t entry_pc,
                                            uint32_t inject_dst,
                                            TaggedValue inject) {
    if (call_depth_ >= config_.max_call_depth) {
        return fail(ErrorCode::RuntimeError,
                    "call depth exceeded (" +
                        std::to_string(config_.max_call_depth) + ")");
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

    ++method.runtime.invocation_count;

    if (!method.runtime.offsets_ready) build_offset_maps(method);
    ugb::ModuleRuntimeData& mdm = module.runtime;

    method.ensure_runtime_tables(method.runtime.instruction_offsets.size());

    // ---- frame allocation (Rule 67: zero steady-state allocation) ----------
    // One reusable contiguous register file per call depth; buffers are
    // reused across calls (high-water reuse). Argument windows are contiguous
    // slices of the caller's frame buffer, so callee spans stay valid while
    // the caller frame is parked.
    // BOUNDARY (CEM-26 section 3): the two growth paths below are the only
    // allocation reachable from execute(); both fire at most once per new
    // high-water mark, never per instruction.
    if (args.size() > method.register_count) {
        return fail(ErrorCode::InvalidArgument,
                    "argument window exceeds callee frame");
    }
    if (frames_live_ >= frames_.size()) frames_.emplace_back();
    Frame& frame = frames_[frames_live_++];
    struct FrameGuard {
        size_t& live;
        ~FrameGuard() { --live; }
    } frame_guard{frames_live_};
    if (frame.regs.size() < method.register_count) {
        frame.regs.resize(method.register_count);
    }
    TaggedValue* R = frame.regs.data();
    for (size_t i = 0; i < args.size(); ++i) R[i] = args[i];
    if (inject_dst != 0xFFFFFFFFu) {
        if (static_cast<size_t>(inject_dst) >= method.register_count) {
            return fail(ErrorCode::InvalidArgument,
                        "resume: inject register out of range");
        }
        R[inject_dst] = inject;
    }

    const uint8_t* code = method.code.data();
    const size_t code_size = method.code.size();
    size_t pc = entry_pc;
    Decoded ins;
    Op last_op = Op::ILLEGAL;

    Result<RunResult> exit_result =
        RunResult{TaggedValue::undefined(), InterpStats{}};

// Computed-goto dispatch (docs/tier-t0.md section 4, ADR-002). The dispatch
// table is process-global and immutable after one-time initialization
// (Rule 57: allowed global state = immutable tables). Initialization uses a
// single-writer protocol: the first executor builds the table and publishes
// it with a release store; concurrent executors acquire-spin until
// published, so no thread ever reads a partially built table.
// CEM-26 section 14: acquire/release, never seq_cst — the protocol needs
// publication visibility only, not a total order. See the master
// PERF_PERMIT on execute() for the indirect-jump justification.
#if defined(__GNUC__) && !defined(VORTEX_NO_COMPUTED_GOTO)
// &&label and goto* are documented GNU extensions (ADR-002); the pedantic
// diagnostic is silenced for the entire dispatch region — the init block
// and every VORTEX_DISPATCH()/goto* expansion site below.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#define L(name) L_##name

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
                        std::to_string(opc));                   \
                goto L_done;                                    \
            }                                                   \
            goto* dispatch[opc];                                \
        }                                                       \
    } while (0)

    static void* dispatch[ugb::kOpcodeCount] = {};
    static std::atomic<uint8_t> dispatch_state{
        vortex::cem::kDispatchUninitialized};
    if (dispatch_state.load(std::memory_order::acquire) !=
        vortex::cem::kDispatchReady) {
        uint8_t expected = vortex::cem::kDispatchUninitialized;
        if (dispatch_state.compare_exchange_strong(
                expected, vortex::cem::kDispatchBuilding,
                std::memory_order::acquire, std::memory_order::acquire)) {
            auto* T = dispatch;
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
            MAP(Op::ADD_I32, L(TYPED_ARITH));
            MAP(Op::ADD_I64, L(TYPED_ARITH));
            MAP(Op::ADD_CHECKED_I32, L(TYPED_ARITH));
            MAP(Op::SUB_ANY, L(SUB_ANY));
            MAP(Op::SUB_I32, L(TYPED_ARITH));
            MAP(Op::SUB_I64, L(TYPED_ARITH));
            MAP(Op::ADD_F64, L(ADD_F64));
            MAP(Op::SUB_F64, L(SUB_F64));
            MAP(Op::MUL_F64, L(MUL_F64));
            MAP(Op::MUL_ANY, L(MUL_ANY));
            MAP(Op::MUL_I32, L(TYPED_ARITH));
            MAP(Op::MUL_I64, L(TYPED_ARITH));
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
            dispatch_state.store(vortex::cem::kDispatchReady,
                                 std::memory_order::release);
        } else {
            // Another thread is building; wait for publication.
            while (dispatch_state.load(std::memory_order::acquire) !=
                   vortex::cem::kDispatchReady) {
                std::this_thread::yield();
            }
        }
    }

    // Chronic-failure path shared by every typed (speculative) handler
    // (Rule 34: specializations must fall back, never trap without escape).
    // Declared before the first dispatch: handlers may not be reached by a
    // jump that crosses a non-trivial initialization.
    auto note_speculation_failure = [&](Op typed) {
        const uint32_t idx = instruction_index_at(method, pc);
        if (idx != kInvalidInstructionIndex) {
            ProfileSlot& p = method.profiles[idx];
            p.record_failure();
            if (p.failure_count >= config_.generic_rewrite_threshold) {
                maybe_rewrite_to_generic(method, pc, typed);
            }
        }
    };

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
            const uint32_t pidx = instruction_index_at(method, pc);         \
            if (pidx != kInvalidInstructionIndex)                           \
                method.profiles[pidx].record_execution();                   \
        }                                                                   \
    } while (0)

#define VORTEX_ADVANCE()                     \
    do {                                     \
        record_bigram(last_op,               \
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
    R[ins.dst] = TaggedValue::smi(static_cast<int32_t>(ins.meta));
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
    // Canonical forms handle all operand types they define; overflow is a
    // distinct, guest-visible trap (Rule 110: numeric semantics exact).
L_ADD_ANY: {
    VORTEX_PROFILE();
    stats_.generic_instructions_executed++;
    const ArithResult r = generic_add(R[ins.s0], R[ins.s1]);
    if (r.status == ArithStatus::SmiOverflow) {
        VORTEX_RT_ERROR("Add.Any: integer overflow");
    }
    if (r.status != ArithStatus::Ok) {
        VORTEX_RT_ERROR("Add.Any: unsupported operand types");
    }
    R[ins.dst] = r.value;
    // Adaptive: stable smi behavior promotes the site (Rule 22: profile-
    // driven, never timeout-driven).
    if (config_.enable_profiling) {
        const uint32_t idx = instruction_index_at(method, pc);
        if (idx != kInvalidInstructionIndex) {
            ProfileSlot& p = method.profiles[idx];
            if (R[ins.s0].is_smi() && R[ins.s1].is_smi()) {
                p.branch_counts[0]++;
                if (p.branch_counts[0] >= config_.typed_rewrite_threshold) {
                    maybe_rewrite_to_typed(method, pc, Op::ADD_ANY, Op::ADD_I32);
                }
            } else {
                p.branch_counts[0] = 0;
            }
        }
    }
    VORTEX_NEXT();
}
L_SUB_ANY: {
    VORTEX_PROFILE();
    stats_.generic_instructions_executed++;
    const ArithResult r = generic_sub(R[ins.s0], R[ins.s1]);
    if (r.status == ArithStatus::SmiOverflow) {
        VORTEX_RT_ERROR("Sub.Any: integer overflow");
    }
    if (r.status != ArithStatus::Ok) {
        VORTEX_RT_ERROR("Sub.Any: unsupported operand types");
    }
    R[ins.dst] = r.value;
    if (config_.enable_profiling) {
        const uint32_t idx = instruction_index_at(method, pc);
        if (idx != kInvalidInstructionIndex) {
            ProfileSlot& p = method.profiles[idx];
            if (R[ins.s0].is_smi() && R[ins.s1].is_smi()) {
                p.branch_counts[0]++;
                if (p.branch_counts[0] >= config_.typed_rewrite_threshold) {
                    maybe_rewrite_to_typed(method, pc, Op::SUB_ANY, Op::SUB_I32);
                }
            } else {
                p.branch_counts[0] = 0;
            }
        }
    }
    VORTEX_NEXT();
}
L_MUL_ANY: {
    VORTEX_PROFILE();
    stats_.generic_instructions_executed++;
    const ArithResult r = generic_mul(R[ins.s0], R[ins.s1]);
    if (r.status == ArithStatus::SmiOverflow) {
        VORTEX_RT_ERROR("Mul.Any: integer overflow");
    }
    if (r.status != ArithStatus::Ok) {
        VORTEX_RT_ERROR("Mul.Any: unsupported operand types");
    }
    R[ins.dst] = r.value;
    if (config_.enable_profiling) {
        const uint32_t idx = instruction_index_at(method, pc);
        if (idx != kInvalidInstructionIndex) {
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
    }
    VORTEX_NEXT();
}

    // Typed smi arithmetic: speculative fast path; on failure count it,
    // maybe demote, then run the canonical semantics inline (Rule 34).
L_TYPED_ARITH: {
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    const Op self = static_cast<Op>(ins.opcode);
    const bool is_add = self == Op::ADD_I32 || self == Op::ADD_I64 ||
                        self == Op::ADD_CHECKED_I32;
    const bool is_sub = self == Op::SUB_I32 || self == Op::SUB_I64;
    const bool is_mul = self == Op::MUL_I32 || self == Op::MUL_I64;
    if (!(is_add || is_sub || is_mul)) {
        VORTEX_RT_ERROR("bad typed arithmetic opcode");
    }
    if (R[ins.s0].is_smi() && R[ins.s1].is_smi()) {
        const int64_t x = R[ins.s0].as_smi();
        const int64_t y = R[ins.s1].as_smi();
        int64_t r = 0;
        bool ovf = true;
        if (is_add) {
            ovf = __builtin_add_overflow(x, y, &r);
        } else if (is_sub) {
            ovf = __builtin_sub_overflow(x, y, &r);
        } else {
            ovf = __builtin_mul_overflow(x, y, &r);
        }
        if (!ovf && r >= TaggedValue::smi_min() && r <= TaggedValue::smi_max()) {
            R[ins.dst] = TaggedValue::smi(r);
            VORTEX_NEXT();
        }
    }
    note_speculation_failure(self);
    const ArithResult ar = is_add ? generic_add(R[ins.s0], R[ins.s1])
                           : is_sub ? generic_sub(R[ins.s0], R[ins.s1])
                                    : generic_mul(R[ins.s0], R[ins.s1]);
    if (ar.status != ArithStatus::Ok) {
        VORTEX_RT_ERROR(std::string(ugb::opcode_name(self)) +
                        (ar.status == ArithStatus::SmiOverflow
                             ? ": integer overflow"
                             : ": unsupported operand types"));
    }
    R[ins.dst] = ar.value;
    VORTEX_NEXT();
}

    // ---- float arithmetic (Rule 110: IEEE semantics preserved — NaN and
    // signed zero flow; division by zero yields inf/nan, never a substitute).
L_ADD_F64: {
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    double a = 0.0, b = 0.0;
    if (!as_double(R[ins.s0], a) || !as_double(R[ins.s1], b)) {
        VORTEX_RT_ERROR("Add.F64: operand is not a float");
    }
    auto boxed = heap_.allocate_double(a + b);
    if (!boxed) VORTEX_RT_ERROR("allocation failed (double result)");
    stats_.allocations++;
    R[ins.dst] = TaggedValue::heap_pointer(*boxed);
    VORTEX_NEXT();
}
L_SUB_F64: {
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    double a = 0.0, b = 0.0;
    if (!as_double(R[ins.s0], a) || !as_double(R[ins.s1], b)) {
        VORTEX_RT_ERROR("Sub.F64: operand is not a float");
    }
    auto boxed = heap_.allocate_double(a - b);
    if (!boxed) VORTEX_RT_ERROR("allocation failed (double result)");
    stats_.allocations++;
    R[ins.dst] = TaggedValue::heap_pointer(*boxed);
    VORTEX_NEXT();
}
L_MUL_F64: {
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    double a = 0.0, b = 0.0;
    if (!as_double(R[ins.s0], a) || !as_double(R[ins.s1], b)) {
        VORTEX_RT_ERROR("Mul.F64: operand is not a float");
    }
    auto boxed = heap_.allocate_double(a * b);
    if (!boxed) VORTEX_RT_ERROR("allocation failed (double result)");
    stats_.allocations++;
    R[ins.dst] = TaggedValue::heap_pointer(*boxed);
    VORTEX_NEXT();
}
L_DIV_F64: {
    VORTEX_PROFILE();
    double a = 0.0, b = 0.0;
    if (!as_double(R[ins.s0], a) || !as_double(R[ins.s1], b)) {
        VORTEX_RT_ERROR("Div.F64: operand is not a float");
    }
    auto boxed = heap_.allocate_double(a / b);  // IEEE: inf/nan on div-zero
    if (!boxed) VORTEX_RT_ERROR("allocation failed");
    stats_.allocations++;
    R[ins.dst] = TaggedValue::heap_pointer(*boxed);
    VORTEX_NEXT();
}
L_DIV_S_I64: {
    VORTEX_PROFILE();
    if (!R[ins.s0].is_smi() || !R[ins.s1].is_smi()) {
        VORTEX_RT_ERROR("Div.S.I64: non-integer operand");
    }
    if (R[ins.s1].as_smi() == 0) VORTEX_RT_ERROR("division by zero");
    // The single overflow case of two's-complement division: smi_min / -1.
    // Scoped to this handler: a function-scope declaration would put every
    // later goto across an initialization (CEM-26 section 16 hygiene).
    constexpr int64_t kDivOverflowDivisor = -1;
    if (R[ins.s0].as_smi() == TaggedValue::smi_min() &&
        R[ins.s1].as_smi() == kDivOverflowDivisor) {
        VORTEX_RT_ERROR("Div.S.I64: integer overflow");
    }
    R[ins.dst] = TaggedValue::smi(R[ins.s0].as_smi() / R[ins.s1].as_smi());
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
    if (R[ins.s0].as_smi() == TaggedValue::smi_min()) {
        VORTEX_RT_ERROR("Neg.I64: integer overflow");
    }
    R[ins.dst] = TaggedValue::smi(-R[ins.s0].as_smi());
    VORTEX_NEXT();
L_NEG_F64: {
    VORTEX_PROFILE();
    double a = 0.0;
    if (!as_double(R[ins.s0], a)) VORTEX_RT_ERROR("Neg.F64: operand is not a float");
    auto boxed = heap_.allocate_double(-a);
    if (!boxed) VORTEX_RT_ERROR("allocation failed");
    stats_.allocations++;
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
    // Shift amounts are taken modulo the 64-bit width (guest semantics,
    // docs/guest-semantics.md: integer ops); the mask keeps every shift
    // inside the defined-behavior range (CEM-26 section 16: no UB).
    constexpr int64_t kI64ShiftBits = 64;
    constexpr int64_t kI64ShiftMask = kI64ShiftBits - 1;
    const int64_t shift = b & kI64ShiftMask;
    int64_t r = 0;
    switch (static_cast<Op>(ins.opcode)) {
    case Op::AND_I: r = a & b; break;
    case Op::OR_I: r = a | b; break;
    case Op::XOR_I: r = a ^ b; break;
    case Op::SHL_I:
        r = static_cast<int64_t>(static_cast<uint64_t>(a) << shift);
        break;
    case Op::SHR_S_I: r = a >> shift; break;
    case Op::SHR_U_I:
        r = static_cast<int64_t>(static_cast<uint64_t>(a) >> shift);
        break;
    default: VORTEX_RT_ERROR("bad bitwise op");
    }
    // Rule 110 / ADR-005: values outside the Smi range trap, never wrap.
    // (SHL is the only bitwise form that can leave it.)
    if (r < TaggedValue::smi_min() || r > TaggedValue::smi_max()) {
        VORTEX_RT_ERROR("bitwise op: integer overflow");
    }
    R[ins.dst] = TaggedValue::smi(r);
    VORTEX_NEXT();
}
    // ---- comparisons ----------------------------------------------------------
L_CMP: {
    VORTEX_PROFILE();
    const ArithResult r = generic_compare(R[ins.s0], R[ins.s1],
                                          static_cast<Op>(ins.opcode));
    if (r.status != ArithStatus::Ok) {
        VORTEX_RT_ERROR("comparison: unsupported operand types");
    }
    R[ins.dst] = r.value;
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
    double a = 0.0, b = 0.0;
    if (!as_double(R[ins.s0], a) || !as_double(R[ins.s1], b)) {
        VORTEX_RT_ERROR("Eq.F64: operand is not a float");
    }
    R[ins.dst] = TaggedValue::boolean(a == b);  // IEEE equality (NaN != NaN)
    VORTEX_NEXT();
}
L_CMP_F64: {
    VORTEX_PROFILE();
    double a = 0.0, b = 0.0;
    if (!as_double(R[ins.s0], a) || !as_double(R[ins.s1], b)) {
        VORTEX_RT_ERROR("float compare: not a float");
    }
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
    stats_.allocations++;
    R[ins.dst] = TaggedValue::heap_pointer(*boxed);
    VORTEX_NEXT();
}
L_F64_TO_I64: {
    VORTEX_PROFILE();
    double a = 0.0;
    if (!as_double(R[ins.s0], a)) VORTEX_RT_ERROR("F64ToI64: operand is not a float");
    // Saturating conversion (docs/guest-semantics.md, Float->Int): NaN -> 0,
    // out-of-range clamps to the Smi bounds. static_cast would be UB here
    // (Rule 110: defined numeric semantics, no UB on guest inputs).
    int64_t r = 0;
    if (a != a) {
        r = 0;
    } else if (a >= static_cast<double>(TaggedValue::smi_max())) {
        r = TaggedValue::smi_max();
    } else if (a <= static_cast<double>(TaggedValue::smi_min())) {
        r = TaggedValue::smi_min();
    } else {
        r = static_cast<int64_t>(a);
    }
    R[ins.dst] = TaggedValue::smi(r);
    VORTEX_NEXT();
}

    // ---- control flow -----------------------------------------------------------
L_JUMP:
    VORTEX_PROFILE();
    if (ins.meta <= pc) {  // backward branch: profile + tiering handoff
        record_backedge(module, method, static_cast<uint32_t>(pc));
    }
    pc = ins.meta;
    VORTEX_REDIRECT();
L_JUMP_TRUE: {
    VORTEX_PROFILE();
    const bool taken = R[ins.s0].truthy();
    if (config_.enable_profiling) {
        const uint32_t idx = instruction_index_at(method, pc);
        if (idx != kInvalidInstructionIndex) {
            method.profiles[idx].record_branch(taken);
        }
    }
    if (taken) {
        if (ins.meta <= pc) {
            record_backedge(module, method, static_cast<uint32_t>(pc));
        }
        pc = ins.meta;
        VORTEX_REDIRECT();
    }
    VORTEX_NEXT();
}
L_JUMP_FALSE: {
    VORTEX_PROFILE();
    const bool taken = !R[ins.s0].truthy();
    if (config_.enable_profiling) {
        const uint32_t idx = instruction_index_at(method, pc);
        if (idx != kInvalidInstructionIndex) {
            method.profiles[idx].record_branch(taken);
        }
    }
    if (taken) {
        if (ins.meta <= pc) {
            record_backedge(module, method, static_cast<uint32_t>(pc));
        }
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
    // Call arguments are a contiguous window of the caller frame: the callee
    // receives a span into the frame pool — no per-call heap allocation
    // (Rule 67). Pool growth for nested frames never relocates elements.
L_CALL_DIRECT: {
    VORTEX_PROFILE();
    stats_.calls++;
    const int32_t target = resolve_method(module, ins.meta);
    if (target < 0) VORTEX_RT_ERROR("Call.Direct: unresolved method token");
    if (static_cast<size_t>(ins.s0) + ins.s1 > method.register_count) {
        VORTEX_RT_ERROR("Call.Direct: argument window out of range");
    }
    ugb::UGBMethod& callee =
        module.method_table[static_cast<size_t>(target)];
    auto sub = execute(module, callee,
                       std::span<const TaggedValue>(&R[ins.s0], ins.s1));
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
    // Dispatch through the receiver's klass method table (docs/ugb.md 4.3),
    // cached by (klass id, method token) — no string compares on the hot
    // path (Rule 5).
    if (ins.s0 >= method.register_count || !R[ins.s0].is_heap_object()) {
        VORTEX_RT_ERROR("Call.Virtual: receiver is not an object");
    }
    auto* recv = R[ins.s0].as_heap_object();
    if (recv->header.klass == nullptr) {
        VORTEX_RT_ERROR("Call.Virtual: receiver has no klass");
    }
    const uint32_t tok_idx = ins.meta;  // token: 1-based, 0 = none
    if (tok_idx == 0 || tok_idx > module.methods.size()) {
        VORTEX_RT_ERROR("Call.Virtual: bad method token");
    }
    const uint32_t klass_id = recv->header.klass->id();
    const uint64_t vkey = klass_token_key(klass_id, tok_idx);
    int32_t impl = kNoSlot;
    if (int32_t* hit = mdm.virtual_resolution.find(vkey)) {
        impl = *hit;
    } else {
        // Cold path: name-based resolution fills the cache once (Rule 5).
        auto* entry = recv->header.klass->find_method(
            module.methods[tok_idx - 1].name);
        if (entry == nullptr ||
            entry->kind != MethodImplementationKind::Bytecode) {
            VORTEX_RT_ERROR(
                "Call.Virtual: no bytecode implementation on receiver");
        }
        impl = static_cast<int32_t>(entry->ugb_method_id);
        mdm.virtual_resolution.insert(vkey, impl);
    }
    if (static_cast<size_t>(ins.s0) + ins.s1 > method.register_count) {
        VORTEX_RT_ERROR("Call.Virtual: argument window out of range");
    }
    ugb::UGBMethod& callee =
        module.method_table[static_cast<size_t>(impl)];
    auto sub = execute(module, callee,
                       std::span<const TaggedValue>(&R[ins.s0], ins.s1));
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
    if (static_cast<size_t>(ins.s0) + ins.s1 > method.register_count) {
        VORTEX_RT_ERROR("Call.Builtin: argument window out of range");
    }
    // PERF_PERMIT PERF-002 (indirect call through a function pointer, CEM-26
    // section 7/10):
    // REASON: the builtin ABI is the host-extension boundary (Rule 5:
    //         token-keyed; hosts register native sinks). Builtins are
    //         guest-visible operations (print, math intrinsics) — the call
    //         is the operation, not per-item overhead inside a loop.
    // COST: one indirect call per CALL_BUILTIN execution; branch target is
    //       stable per site (mono builtin resolution caches by token), so
    //       the BTB predicts it after first execution.
    // OWNER: @vortex/rt (registered in docs/cem26.md section 5)
    R[ins.dst] = b.fn(std::span<const TaggedValue>(&R[ins.s0], ins.s1), b.user);
    VORTEX_NEXT();
}

    // ---- allocation / fields / arrays -------------------------------------------------
L_NEW_OBJECT: {
    VORTEX_PROFILE();
    if (ins.meta >= mdm.klass_table.size()) {
        VORTEX_RT_ERROR("New.Object: bad klass token");
    }
    auto* k = static_cast<Klass*>(mdm.klass_table[ins.meta]);
    auto obj = heap_.allocate_object(k, k->field_count());
    if (!obj) VORTEX_RT_ERROR("allocation failed (object)");
    stats_.allocations++;
    R[ins.dst] = TaggedValue::heap_pointer(*obj);
    VORTEX_NEXT();
}
L_NEW_ARRAY: {
    VORTEX_PROFILE();
    if (!R[ins.s0].is_smi() || R[ins.s0].as_smi() < 0 ||
        R[ins.s0].as_smi() > static_cast<int64_t>(config_.max_array_length)) {
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
    // Inline cache first (docs/tier-t0.md section 3): mono/poly hit -> direct
    // slot index. Miss falls back to the ID-keyed cache, which fills the IC.
    const uint32_t klass_id = o->header.klass->id();
    const uint32_t idx = instruction_index_at(method, pc);
    if (idx != kInvalidInstructionIndex) {
        IcSlot& ic = method.ics[idx];
        if (const ugb::IcEntry* e = ic.lookup(klass_id)) {
            stats_.ic_hits++;
            R[ins.dst] = static_cast<Object*>(o)->field(e->target);
            VORTEX_NEXT();
        }
        stats_.ic_misses++;
    }
    int32_t slot = kNoSlot;
    TaggedValue v;
    if (!get_field_cached(obj, ins.meta, module, v, &slot)) {
        VORTEX_RT_ERROR("GetField: unresolved field");
    }
    if (slot >= 0 && idx != kInvalidInstructionIndex) {
        method.ics[idx].record_hit(klass_id, static_cast<uint32_t>(slot));
    }
    R[ins.dst] = v;
    VORTEX_NEXT();
}
L_GET_FIELD_SHAPE: {
    // Speculative form: shape guard + offset load, fallback to canonical.
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    const TaggedValue obj = R[ins.s0];
    const uint32_t idx = instruction_index_at(method, pc);
    if (obj.is_heap_object() &&
        obj.as_heap_object()->header.klass != nullptr &&
        idx != kInvalidInstructionIndex) {
        const uint32_t klass_id = obj.as_heap_object()->header.klass->id();
        IcSlot& ic = method.ics[idx];
        if (const ugb::IcEntry* e = ic.lookup(klass_id)) {
            stats_.ic_hits++;
            R[ins.dst] =
                static_cast<Object*>(obj.as_heap_object())->field(e->target);
            VORTEX_NEXT();
        }
    }
    // Guard failed -> canonical fallback (Rule 34); chronic failure demotes
    // the opcode (Rule 43: failure counting feeds tiering decisions).
    note_speculation_failure(Op::GET_FIELD_SHAPE);
    int32_t slot = kNoSlot;
    TaggedValue v;
    if (!get_field_cached(obj, ins.meta, module, v, &slot)) {
        VORTEX_RT_ERROR("GetField.Shape: guard failed, field unresolved");
    }
    if (slot >= 0 && idx != kInvalidInstructionIndex && obj.is_heap_object() &&
        obj.as_heap_object()->header.klass != nullptr) {
        method.ics[idx].record_hit(
            obj.as_heap_object()->header.klass->id(),
            static_cast<uint32_t>(slot));
    }
    R[ins.dst] = v;
    VORTEX_NEXT();
}
L_SET_FIELD: {
    VORTEX_PROFILE();
    if (!set_field_cached(R[ins.s0], R[ins.s1], ins.meta, module)) {
        VORTEX_RT_ERROR("SetField: unresolved field or bad receiver");
    }
    VORTEX_NEXT();
}
L_SET_FIELD_SHAPE: {
    VORTEX_PROFILE();
    stats_.typed_instructions_executed++;
    if (!set_field_cached(R[ins.s0], R[ins.s1], ins.meta, module)) {
        note_speculation_failure(Op::SET_FIELD_SHAPE);
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
                          static_cast<Klass*>(mdm.klass_table[ins.meta]);
    if (!pass) VORTEX_RT_ERROR("Check.Class: class guard failed");
    VORTEX_NEXT();
}
L_CHECK_BOUNDS: {
    VORTEX_PROFILE();
    if (!R[ins.s0].is_smi() || !R[ins.s1].is_smi()) {
        VORTEX_RT_ERROR("Check.Bounds: non-integer index/length");
    }
    // Signed checks first: a negative index or length would wrap under the
    // unsigned fast path and silently pass the guard.
    const int64_t idx_s = R[ins.s0].as_smi();
    const int64_t len_s = R[ins.s1].as_smi();
    if (idx_s < 0 || len_s < 0) {
        VORTEX_RT_ERROR("Check.Bounds: negative index or length");
    }
    if (idx_s >= len_s) VORTEX_RT_ERROR("Check.Bounds: out of bounds");
    VORTEX_NEXT();
}

    // ---- misc --------------------------------------------------------------------
L_SAFEPOINT_POLL:
    VORTEX_PROFILE();
    stats_.safepoint_polls++;
    // Rule 81: safepoints must be reachable within bounded time. The hook is
    // installed by the suspension infra in M2+; the poll site itself is
    // already correct.
    // PERF_PERMIT PERF-003 (indirect call through a function pointer,
    // CEM-26 section 7/10):
    // REASON: the suspension handshake must be replaceable without
    //         recompiling the engine (Rule 81); a data-oriented alternative
    //         (flag + atomic poll word) lands with the M2 threading infra
    //         and this site converts to it then.
    // COST: one null test + (rare) indirect call per SAFEPOINT_POLL
    //       execution; the null test is the common case in M0 (no hook).
    // OWNER: @vortex/rt (registered in docs/cem26.md section 5; expiry M2)
    if (safepoint_hook_ != nullptr) safepoint_hook_(safepoint_hook_user_);
    VORTEX_NEXT();
L_NOP:
    VORTEX_NEXT();

L_done:
    if (!exit_result.has_value()) return std::unexpected(exit_result.error());
    return exit_result;

#else
    // Switch-based fallback: the M0 dispatch relies on the computed-goto
    // extension. Other toolchains degrade safely with an explicit error
    // (Rule 29: degrade, never crash) — see ADR-002 and EXC-002 in the
    // exception register.
    (void)code;
    (void)code_size;
    (void)pc;
    (void)ins;
    (void)last_op;
    (void)exit_result;
    (void)mdm;
    return fail(ErrorCode::Unimplemented,
                "T0 dispatch requires computed goto (GCC/Clang); see ADR-002 "
                "and exception register entry EXC-002");
#endif
#pragma GCC diagnostic pop
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

// @cold — fires only when a backedge count crosses a tiering threshold
// (edge-triggered, docs/tiering.hpp). Between crossings this function is one
// increment plus two counter comparisons at the call sites.
void Interpreter::record_backedge(ugb::UGBModule& module,
                                  ugb::UGBMethod& method, uint32_t pc) {
    (void)module;
    ugb::MethodRuntimeData& rt = method.runtime;
    ++rt.backedge_count;

    // Rule 22: promotion is a deterministic function of heat counters, never
    // wall-clock time. Rule 28: every decision is recorded and observable.
    MethodHotness h;
    h.invocations = rt.invocation_count;
    h.backedges = rt.backedge_count;
    h.deopts = 0;
    h.current = Tier::T0;

    if (tiering_.should_osr(h)) {
        ++stats_.osr_requests;
        record_transition(method.id, pc, Tier::T0, Tier::J1,
                          TierTransitionRecord::Kind::OsrRequest,
                          "loop backedge heat (OSR)");
        if (tiering_.observer() != nullptr) {
            tiering_.observer()->on_osr_request(method.id, pc, Tier::J1);
        }
    }
    const Tier promoted = tiering_.evaluate(h);
    if (promoted != Tier::T0) {
        record_transition(method.id, pc, Tier::T0, promoted,
                          TierTransitionRecord::Kind::Promote,
                          "invocation threshold crossed");
        if (tiering_.observer() != nullptr) {
            tiering_.observer()->on_promote(method.id, Tier::T0, promoted);
        }
    }
}

// @cold — name-resolution caches: first call per token, then an array index
// (method_resolution/builtin_slot are direct-indexed u32 tables).
int32_t Interpreter::resolve_method(ugb::UGBModule& module, uint32_t token) const {
    ugb::ModuleRuntimeData& rt = module.runtime;
    if (!rt.ready) return kNoSlot;
    if (token == 0 || token >= rt.method_resolution.size()) return kNoSlot;
    int32_t cached = rt.method_resolution[token];
    if (cached >= 0) return cached;
    const std::string& name = module.methods[token - 1].name;
    cached = module.find_method(name);
    rt.method_resolution[token] = cached;
    return cached;
}

// @cold — see resolve_method.
Result<TaggedValue> Interpreter::invoke_builtin_token(
    ugb::UGBModule& module, uint32_t token,
    std::span<const TaggedValue> args) {
    const int32_t slot = resolve_builtin(module, token);
    if (slot < 0) {
        return support::fail(support::ErrorCode::RuntimeError,
                             "Call.Builtin: unresolved builtin token");
    }
    const Builtin& b = builtins_[static_cast<size_t>(slot)];
    return b.fn(args, b.user);
}

int32_t Interpreter::resolve_builtin(ugb::UGBModule& module, uint32_t token) const {
    ugb::ModuleRuntimeData& rt = module.runtime;
    if (!rt.ready || token >= rt.builtin_slot.size()) return kNoSlot;
    int32_t cached = rt.builtin_slot[token];
    if (cached >= 0) return cached;
    const std::string& name = module.builtins[token].name;
    for (size_t i = 0; i < builtins_.size(); ++i) {
        if (builtins_[i].name == name) {
            rt.builtin_slot[token] = static_cast<int32_t>(i);
            return static_cast<int32_t>(i);
        }
    }
    return kNoSlot;
}

// @hot
// PERF_CONTRACT:
// BUDGET: <= 5 cycles (1 klass-word load + 1 klass-compare + 1 memcpy'd
//         double load of 8 bytes from the object body)
// READS: 16 bytes (header klass word + payload), same object, same line
// WRITES: 0 (double returned in xmm)
// BRANCHES: 2 (heap-pointer test, klass identity — both predicted in
//           float-typed loops)
// CACHE: one object line; boxed doubles are 24 bytes so header+payload are
//        line-co-resident by construction
//
// PERF_OBSERVATION:
// TARGET: Zen 5 / ARM Neoverse V2
// VALIDATED: g++ 14.2, -O2 -fno-rtti
// ACTUAL: PENDING microbench (M0) — see docs/cem26.md section 4
// LAST_VALIDATED: 2026-09-22
bool Interpreter::as_double(TaggedValue v, double& out) const noexcept {
    if (!v.is_heap_object()) return false;
    auto* o = v.as_heap_object();
    if (!is_boxed_double(o, heap_.double_klass())) return false;
    out = read_boxed_double(o);
    return true;
}

}  // namespace vortex::vm

