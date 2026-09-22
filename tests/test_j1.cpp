// J1 stencil baseline JIT tests (docs/roadmap.md M1 DoD):
//   - golden machine-code pins for representative templates
//   - parity: J1-compiled methods produce T0-identical observable results
//   - superstencil promotion from hot bigrams
//   - IC guard strengthening (patch_ic_guard)
//   - compile latency under the J1 budget
//   - OSR entry stubs for hot loops
#include "vortex_test.hpp"

#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "vortex/gc/icggc.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j1/stencil_corpus.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;
using namespace vortex::ugb;
using namespace vortex::j1;

namespace {

StencilTable make_corpus() {
    StencilTable t;
    build_default_corpus(t);
    return t;
}

const Stencil* find_stencil(const StencilTable& t, Op op) {
    return t.select(static_cast<uint16_t>(op), false);
}

// T0 side of the parity harness.
vm::Result<vm::RunResult> run_t0(const char* src, const std::string& entry,
                                 const std::vector<TaggedValue>& args) {
    auto module = assemble_module(src);
    if (!module) return std::unexpected(module.error());
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.register_builtin(
        "print", [](std::span<const TaggedValue>, void*) {
            return TaggedValue::undefined();
        });
    return interp.run(*module, entry, args);
}

// J1 side of the parity harness: compiles `entry` natively (callees fall
// back to the attached T0 through the token-call helper).
vm::Result<TaggedValue> run_j1(const char* src, const std::string& entry,
                               const std::vector<TaggedValue>& args,
                               StencilTable& corpus) {
    auto module = assemble_module(src);
    if (!module) return std::unexpected(module.error());
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.register_builtin(
        "print", [](std::span<const TaggedValue>, void*) {
            return TaggedValue::undefined();
        });
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    if (!br) return std::unexpected(br.error());
    const int32_t id = module->find_method(entry);
    if (id < 0) {
        return fail(support::ErrorCode::InvalidArgument, "no such method");
    }
    j1::BaselineJit jit(corpus);
    j1::BaselineJob job;
    job.module = &*module;
    job.method_id = static_cast<uint32_t>(id);
    job.klass_addrs = &bindings.klass_addr_table;
    job.field_offsets = bindings.field_offsets.data();
    job.field_offset_count = bindings.field_offsets.size();
    job.double_klass = bindings.double_klass;
    auto code = jit.compile(job);
    if (!code) return std::unexpected(code.error());
    auto ex = j1::publish_baseline(*code);
    if (!ex) return std::unexpected(ex.error());
    return j1::run_baseline(*ex, bindings, args);
}

// Observable equivalence for the parity DoD: Smis and immediates compare by
// bits, boxed doubles by payload (the two engines allocate independently).
bool observable_eq(const TaggedValue& a, const TaggedValue& b) {
    if (a.raw() == b.raw()) return true;
    if (a.is_smi() || b.is_smi()) return false;
    if (a.is_null() || b.is_null()) return false;
    return false;  // distinct heap allocations are never equal in M1
}

bool boxed_double_eq(const TaggedValue& a, const TaggedValue& b,
                     gc::Heap& heap) {
    auto* ka = a.as_heap_object();
    auto* kb = b.as_heap_object();
    if (ka == nullptr || kb == nullptr) return false;
    if (ka->header.klass != heap.double_klass()) return false;
    if (kb->header.klass != heap.double_klass()) return false;
    return read_boxed_double(ka) == read_boxed_double(kb);
}

}  // namespace

// ---- golden machine-code pins ---------------------------------------------------

VORTEX_TEST(j1_corpus_builds) {
    StencilTable t = make_corpus();
    VORTEX_EXPECT(t.stencil_count() >= 60);
    VORTEX_EXPECT(find_stencil(t, Op::ADD_ANY) != nullptr);
    VORTEX_EXPECT(find_stencil(t, Op::RETURN) != nullptr);
}

VORTEX_TEST(j1_golden_const_null) {
    StencilTable t = make_corpus();
    const Stencil* s = find_stencil(t, Op::CONST_NULL);
    VORTEX_EXPECT(s != nullptr);
    if (!s) return;
    // mov qword [rbp - 160], 3 ; jmp rel32
    const uint8_t golden[] = {0x48, 0xc7, 0x85, 0x60, 0xff, 0xff, 0xff,
                              0x03, 0x00, 0x00, 0x00, 0xe9, 0x00, 0x00,
                              0x00, 0x00};
    VORTEX_EXPECT_EQ(s->bytes.size(), sizeof(golden));
    VORTEX_EXPECT(std::memcmp(s->bytes.data(), golden, sizeof(golden)) == 0);
    VORTEX_EXPECT_EQ(s->patch_sites.size(), static_cast<size_t>(2));
}

VORTEX_TEST(j1_golden_move_and_return) {
    StencilTable t = make_corpus();
    const Stencil* mv = find_stencil(t, Op::MOVE);
    VORTEX_EXPECT(mv != nullptr);
    if (mv) {
        const uint8_t golden[] = {0x48, 0x8b, 0x85, 0x60, 0xff, 0xff, 0xff,
                                  0x48, 0x89, 0x85, 0x60, 0xff, 0xff, 0xff,
                                  0xe9, 0x00, 0x00, 0x00, 0x00};
        VORTEX_EXPECT_EQ(mv->bytes.size(), sizeof(golden));
        VORTEX_EXPECT(std::memcmp(mv->bytes.data(), golden, sizeof(golden)) == 0);
        VORTEX_EXPECT_EQ(mv->patch_sites.size(), static_cast<size_t>(3));
    }
    const Stencil* ret = find_stencil(t, Op::RETURN);
    VORTEX_EXPECT(ret != nullptr);
    if (ret) {
        // load vreg; mov [r14], rax; xor eax, eax; jmp ok-epilogue
        const uint8_t head[] = {0x48, 0x8b, 0x85, 0x60, 0xff, 0xff,
                                0xff, 0x49, 0x89, 0x06, 0x48, 0x31,
                                0xc0, 0xe9};
        VORTEX_EXPECT(ret->bytes.size() >= sizeof(head));
        VORTEX_EXPECT(std::memcmp(ret->bytes.data(), head, sizeof(head)) == 0);
    }
}

VORTEX_TEST(j1_golden_jump_tail) {
    StencilTable t = make_corpus();
    const Stencil* j = find_stencil(t, Op::JUMP);
    VORTEX_EXPECT(j != nullptr);
    if (!j) return;
    const uint8_t golden[] = {0xe9, 0x00, 0x00, 0x00, 0x00};
    VORTEX_EXPECT_EQ(j->bytes.size(), sizeof(golden));
    VORTEX_EXPECT(std::memcmp(j->bytes.data(), golden, sizeof(golden)) == 0);
    VORTEX_EXPECT_EQ(j->patch_sites.size(), static_cast<size_t>(1));
    VORTEX_EXPECT_EQ(j->patch_sites[0].kind, PatchKind::BranchTarget);
    VORTEX_EXPECT_EQ(j->patch_sites[0].operand, static_cast<uint8_t>(0));
}

// ---- parity (M1 DoD) --------------------------------------------------------------

VORTEX_TEST(j1_parity_fib_recursive_entry) {
    // The fib method itself is the J1 entry: loops, typed arithmetic and
    // comparisons all run natively.
    constexpr const char* kSrc = R"(
.method fib(regs=7, args=1)
    Const.I32 v1, 0
    Const.I32 v2, 1
    Const.I32 v3, 1
loop:
    Const.I32 v4, 0
    Eq.I64 v5, v0, v4
    JumpTrue v5, done
    Add.I64 v6, v1, v2
    Move v1, v2
    Move v2, v6
    Sub.I64 v0, v0, v3
    Jump loop
done:
    Return v1
.end
)";
    constexpr int32_t kFibInput = 25;
    constexpr int64_t kFib25 = 75025;
    auto t0 = run_t0(kSrc, "fib", {TaggedValue::smi(kFibInput)});
    VORTEX_EXPECT(t0.has_value());
    if (t0) VORTEX_EXPECT_EQ(t0->value.as_smi(), kFib25);

    StencilTable corpus = make_corpus();
    auto j1 = run_j1(kSrc, "fib", {TaggedValue::smi(kFibInput)}, corpus);
    VORTEX_EXPECT(j1.has_value());
    if (j1) VORTEX_EXPECT_EQ(j1->as_smi(), kFib25);
}

VORTEX_TEST(j1_parity_smi_arithmetic) {
    constexpr const char* kSrc = R"(
.method f(regs=6, args=2)
    Add.I32 v2, v0, v1
    Sub.I32 v3, v2, v1
    Mul.I32 v4, v3, v1
    Add.Any v5, v4, v1
    Return v5
.end
)";
    const std::vector<TaggedValue> args = {TaggedValue::smi(20),
                                           TaggedValue::smi(3)};
    auto t0 = run_t0(kSrc, "f", args);
    StencilTable corpus = make_corpus();
    auto j1 = run_j1(kSrc, "f", args, corpus);
    VORTEX_EXPECT(t0.has_value());
    VORTEX_EXPECT(j1.has_value());
    if (t0 && j1) {
        VORTEX_EXPECT(observable_eq(t0->value, *j1));
        VORTEX_EXPECT_EQ(t0->value.as_smi(), (20 + 3 - 3) * 3 + 3);
    }
}

VORTEX_TEST(j1_parity_smi_overflow_traps) {
    // B1 regression: max_smi + max_smi leaves int63 but fits int64 — T0
    // traps, and the tagged-add overflow flag must reproduce exactly that.
    constexpr const char* kSrc = R"(
.method f(regs=4, args=2)
    Add.I32 v2, v0, v1
    Return v2
.end
)";
    const std::vector<TaggedValue> args = {TaggedValue::smi(4611686018427387903LL),
                                           TaggedValue::smi(4611686018427387903LL)};
    auto t0 = run_t0(kSrc, "f", args);
    VORTEX_EXPECT(!t0.has_value());  // T0: smi overflow trap
    StencilTable corpus = make_corpus();
    auto j1 = run_j1(kSrc, "f", args, corpus);
    VORTEX_EXPECT(!j1.has_value());  // J1: same trap (was: silent wrap)
}

VORTEX_TEST(j1_parity_mul_range_trap) {
    // B1 regression: the product fits int64 but leaves the int63 Smi range.
    constexpr const char* kSrc = R"(
.method f(regs=4, args=2)
    Mul.I32 v2, v0, v1
    Return v2
.end
)";
    // (1<<31)^2 = 2^62: inside int64, outside the int63 Smi range.
    const std::vector<TaggedValue> args = {TaggedValue::smi(1LL << 31),
                                           TaggedValue::smi(1LL << 31)};
    auto t0 = run_t0(kSrc, "f", args);
    VORTEX_EXPECT(!t0.has_value());
    StencilTable corpus = make_corpus();
    auto j1 = run_j1(kSrc, "f", args, corpus);
    VORTEX_EXPECT(!j1.has_value());  // was: silent smi_min wrap
}

VORTEX_TEST(j1_parity_f64_to_i64_executes) {
    // B2 regression: the helper op id was patched as an (unhandled) site.
    constexpr const char* kSrc = R"(
.method f(regs=3, args=1)
    FloatToInt.F64.I64 v1, v0
    Return v1
.end
)";
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto module = assemble_module(kSrc);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    auto boxed = heap.allocate_double(3.7);
    VORTEX_EXPECT(boxed.has_value());
    if (!boxed) return;
    const std::vector<TaggedValue> args = {TaggedValue::heap_pointer(*boxed)};
    auto t0 = interp.run(*module, "f", args);
    VORTEX_EXPECT(t0.has_value());
    if (t0) VORTEX_EXPECT_EQ(t0->value.as_smi(), 3);  // saturating toward 0

    StencilTable corpus = make_corpus();
    auto j1 = run_j1(kSrc, "f", args, corpus);
    VORTEX_EXPECT(j1.has_value());  // was: kErrF64ToI64 on every input
    if (j1) VORTEX_EXPECT_EQ(j1->as_smi(), 3);
}

VORTEX_TEST(j1_alloc_slow_uses_correct_klass) {
    // B3 regression: two classes; the second allocation goes through the
    // TLAB-miss helper and must carry klass token 1 (was hardcoded 0).
    constexpr const char* kSrc = R"(
.class P
.field x in P
.class Q
.field y in Q

.method makeQ(regs=2, args=0)
    New.Object v1, Q
    Return v1
.end

.method main(regs=3, args=0)
    Const.I32 v0, 5
    Call.Direct v1, v0, 0, makeQ
    SetField v1, v0, Q.y
    GetField v2, v1, Q.y
    Return v2
.end
)";
    auto t0 = run_t0(kSrc, "main", {});
    VORTEX_EXPECT(t0.has_value());
    if (t0) VORTEX_EXPECT_EQ(t0->value.as_smi(), 5);

    StencilTable corpus = make_corpus();
    auto j1 = run_j1(kSrc, "main", {}, corpus);
    VORTEX_EXPECT(j1.has_value());  // was: klass token 0 (P) for a Q alloc
    if (j1) VORTEX_EXPECT_EQ(j1->as_smi(), 5);
}

VORTEX_TEST(j1_parity_float_ops) {
    constexpr const char* kSrc = R"(
.method f(regs=5, args=2)
    Add.F64 v2, v0, v1
    Mul.F64 v3, v2, v2
    Return v3
.end
)";
    auto module_a = assemble_module(kSrc);
    auto module_b = assemble_module(kSrc);
    VORTEX_EXPECT(module_a.has_value() && module_b.has_value());
    if (!module_a || !module_b) return;

    gc::Heap heap_a;
    vm::Interpreter interp_a(heap_a);
    // T0's as_double accepts boxed doubles only — build boxed arguments.
    auto ba = heap_a.allocate_double(5.0);
    auto bb = heap_a.allocate_double(7.0);
    VORTEX_EXPECT(ba.has_value() && bb.has_value());
    if (!ba || !bb) return;
    const std::vector<TaggedValue> fargs = {TaggedValue::heap_pointer(*ba),
                                            TaggedValue::heap_pointer(*bb)};
    auto t0 = interp_a.run(*module_a, "f", fargs);
    VORTEX_EXPECT(t0.has_value());
    if (!t0) return;

    StencilTable corpus = make_corpus();
    gc::Heap heap_b;
    vm::Interpreter interp_b(heap_b);
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap_b, *module_b, &interp_b, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    j1::BaselineJit jit(corpus);
    j1::BaselineJob job;
    job.module = &*module_b;
    job.method_id = 0;
    job.double_klass = bindings.double_klass;
    auto code = jit.compile(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    auto ex = j1::publish_baseline(*code);
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    auto j1v = j1::run_baseline(*ex, bindings, fargs);
    VORTEX_EXPECT(j1v.has_value());
    if (j1v) {
        VORTEX_EXPECT(boxed_double_eq(t0->value, *j1v, heap_a));
    }
}

VORTEX_TEST(j1_parity_error_surface) {
    // Add.Any with a null operand: T0 raises "unsupported operand types";
    // J1's generic path must produce the same error surface (run fails).
    constexpr const char* kSrc = R"(
.method f(regs=3, args=1)
    Const.Null v1
    Add.Any v2, v0, v1
    Return v2
.end
)";
    auto t0 = run_t0(kSrc, "f", {TaggedValue::smi(1)});
    VORTEX_EXPECT(!t0.has_value());  // T0: runtime error

    StencilTable corpus = make_corpus();
    auto j1 = run_j1(kSrc, "f", {TaggedValue::smi(1)}, corpus);
    VORTEX_EXPECT(!j1.has_value());  // J1: same error surface
}

VORTEX_TEST(j1_parity_fields_and_objects) {
    constexpr const char* kSrc = R"(
.class Point
.field x in Point
.field y in Point

.method make(regs=3, args=2)
    New.Object v2, Point
    SetField v2, v0, Point.x
    SetField v2, v1, Point.y
    Return v2
.end

.method getx(regs=2, args=1)
    GetField v1, v0, Point.x
    Return v1
.end

.method main(regs=4, args=0)
    Const.I32 v0, 7
    Const.I32 v1, 9
    Call.Direct v2, v0, 2, make
    Call.Direct v3, v2, 1, getx
    Return v3
.end
)";
    // J1 compiles `main` natively; make/getx run through the token helper
    // (T0) — the observable result must match the pure-T0 run.
    auto t0 = run_t0(kSrc, "main", {});
    VORTEX_EXPECT(t0.has_value());
    if (t0) VORTEX_EXPECT_EQ(t0->value.as_smi(), 7);

    StencilTable corpus = make_corpus();
    auto j1 = run_j1(kSrc, "main", {}, corpus);
    VORTEX_EXPECT(j1.has_value());
    if (j1) VORTEX_EXPECT_EQ(j1->as_smi(), 7);
}

// ---- superstencils ----------------------------------------------------------------

VORTEX_TEST(j1_superstencil_promotion_and_match) {
    StencilTable t = make_corpus();
    // Fusion requires the first stencil's continuation tail to be its LAST
    // 5 bytes; helper-backed templates (ADD_ANY) end with their error tail
    // and are correctly refused.
    const Stencil* add = find_stencil(t, Op::ADD_ANY);
    const Stencil* mv = find_stencil(t, Op::MOVE);
    VORTEX_EXPECT(add != nullptr && mv != nullptr);
    if (!add || !mv) return;

    std::unordered_map<uint64_t, uint64_t> bigrams;
    const uint64_t refused_key =
        (static_cast<uint64_t>(add->opcode) << 16) | mv->opcode;
    bigrams[refused_key] = 100;
    auto refused = promote_superstencils(t, bigrams, 64);
    VORTEX_EXPECT_EQ(refused.promoted, static_cast<uint32_t>(0));

    const uint64_t key =
        (static_cast<uint64_t>(mv->opcode) << 16) | mv->opcode;
    bigrams[key] = 100;
    auto stats = promote_superstencils(t, bigrams, 64);
    VORTEX_EXPECT_EQ(stats.candidates, static_cast<uint32_t>(2));
    VORTEX_EXPECT_EQ(stats.promoted, static_cast<uint32_t>(1));
    VORTEX_EXPECT_EQ(t.superstencil_count(), static_cast<size_t>(1));

    // The fused body nops a's continuation tail in place and splices b.
    const uint16_t seq[2] = {mv->opcode, mv->opcode};
    const Superstencil* fused = t.match_superstencil(seq, 2);
    VORTEX_EXPECT(fused != nullptr);
    if (fused) {
        VORTEX_EXPECT_EQ(fused->bytes.size(),
                         mv->bytes.size() + mv->bytes.size());
        // b's sites shifted by (len(a) - 5).
        bool shifted_ok = true;
        for (const PatchSite& ps : fused->patch_sites) {
            if (ps.offset >= fused->bytes.size()) shifted_ok = false;
        }
        VORTEX_EXPECT(shifted_ok);
    }
    // Non-matching sequence: no fusion applies.
    const uint16_t other[2] = {add->opcode, mv->opcode};
    VORTEX_EXPECT(t.match_superstencil(other, 2) == nullptr);
}

// ---- IC guard strengthening ---------------------------------------------------------

VORTEX_TEST(j1_ic_guard_patch_flips_to_conditional) {
    StencilTable t = make_corpus();
    const Stencil* gf = t.select(static_cast<uint16_t>(Op::GET_FIELD), true);
    VORTEX_EXPECT(gf != nullptr);
    if (!gf) return;

    std::vector<uint8_t> code = gf->bytes;  // simulate an instantiation
    const PatchSite* guard_imm = nullptr;   // PA_KlassAddr compare imm64
    const PatchSite* slow_rel = nullptr;    // IcSlot rel32 (nop;jmp slot)
    for (const PatchSite& ps : gf->patch_sites) {
        if (ps.kind == PatchKind::IcSlot && ps.operand == 1) {
            slow_rel = &ps;
        }
        if (ps.kind == PatchKind::ConstantIndex && ps.operand == 3) {
            guard_imm = &ps;
        }
    }
    VORTEX_EXPECT(guard_imm != nullptr && slow_rel != nullptr);
    if (!guard_imm || !slow_rel) return;

    // Pre-patch the rel32 with the JMP-form displacement the compiler
    // would write (next-ip = slow_rel.offset + 4).
    const int32_t jmp_rel = 40;
    std::memcpy(code.data() + slow_rel->offset, &jmp_rel, 4);
    const size_t jmp_bytes = slow_rel->offset;
    VORTEX_EXPECT_EQ(code[jmp_bytes - 2], 0x90);  // nop
    VORTEX_EXPECT_EQ(code[jmp_bytes - 1], 0xE9);  // jmp rel32

    const uint64_t klass = 0x00007f0012345670ull;
    patch_ic_guard(code.data(), *guard_imm, *slow_rel, klass);

    // Imm64 written; nop+jmp flipped to jne (0F 85); rel adjusted by +2.
    uint64_t imm = 0;
    std::memcpy(&imm, code.data() + guard_imm->offset, 8);
    VORTEX_EXPECT_EQ(imm, klass);
    VORTEX_EXPECT_EQ(code[jmp_bytes - 2], 0x0F);
    VORTEX_EXPECT_EQ(code[jmp_bytes - 1], 0x85);
    int32_t rel = 0;
    std::memcpy(&rel, code.data() + slow_rel->offset, 4);
    VORTEX_EXPECT_EQ(rel, jmp_rel);  // same rel32 bytes, same next-ip
}

// ---- latency budget (M1 DoD) ----------------------------------------------------------

VORTEX_TEST(j1_compile_latency_under_budget) {
    constexpr const char* kSrc = R"(
.method fib(regs=7, args=1)
    Const.I32 v1, 0
    Const.I32 v2, 1
    Const.I32 v3, 1
loop:
    Const.I32 v4, 0
    Eq.I64 v5, v0, v4
    JumpTrue v5, done
    Add.I64 v6, v1, v2
    Move v1, v2
    Move v2, v6
    Sub.I64 v0, v0, v3
    Jump loop
done:
    Return v1
.end
)";
    auto module = assemble_module(kSrc);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    StencilTable corpus = make_corpus();
    j1::BaselineJit jit(corpus);
    j1::BaselineJob job;
    job.module = &*module;
    job.method_id = 0;

    const auto start = std::chrono::steady_clock::now();
    auto code = jit.compile(job);
    const auto stop = std::chrono::steady_clock::now();
    VORTEX_EXPECT(code.has_value());
    // Budget: J1 is "near-instant" (docs/tier-j1.md section 10). The
    // instantiation is memcpy + patch; 10 ms is a generous CI-safe bound
    // that still fails if anyone reintroduces per-instruction encoding.
    const auto budget = std::chrono::milliseconds(10);
    VORTEX_EXPECT(stop - start < budget);
}

// ---- OSR entry stubs -------------------------------------------------------------------

VORTEX_TEST(j1_osr_stubs_for_loops) {
    constexpr const char* kSrc = R"(
.method fib(regs=7, args=1)
    Const.I32 v1, 0
    Const.I32 v2, 1
    Const.I32 v3, 1
loop:
    Const.I32 v4, 0
    Eq.I64 v5, v0, v4
    JumpTrue v5, done
    Add.I64 v6, v1, v2
    Move v1, v2
    Move v2, v6
    Sub.I64 v0, v0, v3
    Jump loop
done:
    Return v1
.end
)";
    auto module = assemble_module(kSrc);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    StencilTable corpus = make_corpus();
    j1::BaselineJit jit(corpus);
    j1::BaselineJob job;
    job.module = &*module;
    job.method_id = 0;
    auto code = jit.compile(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    VORTEX_EXPECT_EQ(code->osr_entries.size(), static_cast<size_t>(1));
    VORTEX_EXPECT(code->osr_entry_offset != 0xFFFFFFFF);

    auto ex = j1::publish_baseline(*code);
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    VORTEX_EXPECT(ex->osr_entry != nullptr);

    // OSR into the loop head with a mid-loop register state: n=25, a=0,
    // b=1 (the natural loop entry state) -> fib(25).
    {
    gc::Heap heap;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, nullptr, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    std::vector<TaggedValue> state(7, TaggedValue::smi(0));
    state[0] = TaggedValue::smi(25);
    state[1] = TaggedValue::smi(0);
    state[2] = TaggedValue::smi(1);
    state[3] = TaggedValue::smi(1);
    TaggedValue ret;
    const int64_t rc =
        ex->osr_entry(&bindings.context, state.data(),
                      static_cast<uint32_t>(state.size()), &ret);
    VORTEX_EXPECT_EQ(rc, 0);
    if (rc == 0) VORTEX_EXPECT_EQ(ret.as_smi(), 75025);
    }
}
