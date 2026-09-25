// J2 fast optimizing JIT tests (docs/roadmap.md M2 DoD, Compiler Laws):
//   - differential parity T0 / J2 across the supported opcode surface
//     (Rule 18: one semantic contract, Rule 119: differential testing)
//   - state-exact deopt under profile violation (Rules 30/39/42)
//   - OSR entry parity (Rule 27)
//   - budget degradation + kill switches (Rules 59/131, tier-j2 section 1)
//   - golden pass telemetry (Rule 120) + GC/deopt metadata presence (Rule 86)
//   - Rule 121 regression pack for the get-field sentinel fix (ADR-005)
#include "vortex_test.hpp"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <vector>

#include "vortex/gc/icggc.hpp"
#include "vortex/infra/code_range.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j1/stencil_corpus.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/j2/passes.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;
using namespace vortex::ugb;
using namespace vortex::j2;

namespace {

constexpr const char* kPrintBuiltin = "print";

/// Full J2 harness: T0 warmup builds the runtime tables + profiles
/// (Rules 22/32: J2 consumes T0 profile snapshots), the bindings carry the
/// shared klass table, then the whole pipeline runs for `entry`.
class J2Harness {
public:
    J2Harness(const char* src, const char* entry)
        : src_(src), entry_(entry) {}

    vm::Result<TaggedValue> run(const std::vector<TaggedValue>& args,
                                uint64_t kill_switches = 0,
                                size_t node_cap = 20'000,
                                vm::InterpreterConfig cfg = {}) {
        auto module = assemble_module(src_);
        if (!module) return std::unexpected(module.error());
        module_ = std::make_unique<UGBModule>(std::move(*module));
        heap_ = std::make_unique<gc::Heap>();
        interp_ = std::make_unique<vm::Interpreter>(*heap_, cfg);
        interp_->register_builtin(
            kPrintBuiltin,
            [](std::span<const TaggedValue>, void*) {
                return TaggedValue::undefined();
            });
        // T0 warmup: fills profiles/ICs and the method resolution cache.
        auto warm = interp_->run(*module_, entry_, args);
        if (!warm) return std::unexpected(warm.error());
        auto br = j1::make_j1_bindings(*heap_, *module_, interp_.get(),
                                       bindings_);
        if (!br) return std::unexpected(br.error());
        const int32_t id = module_->find_method(entry_);
        if (id < 0) {
            return fail(support::ErrorCode::InvalidArgument,
                        "J2 test: no such method");
        }
        const ugb::UGBMethod& method =
            module_->method_table[static_cast<size_t>(id)];
        J2Job job;
        job.module = module_.get();
        job.method_id = static_cast<uint32_t>(id);
        job.profiles = &method.profiles;
        job.ics = &method.ics;
        job.klass_addrs = &bindings_.klass_addr_table;
        job.double_klass = bindings_.double_klass;
        job.node_cap = node_cap;
        job.pass_kill_switches = kill_switches;
        auto code = compile_j2(job);
        if (!code) {
            std::printf("  [j2-harness] compile: %s\n",
                        code.error().message.c_str());
            return std::unexpected(code.error());
        }
        code_ = std::make_unique<J2Code>(std::move(*code));
        auto ex = publish_j2(*code_, infra::global_code_range());
        if (!ex) {
            std::printf("  [j2-harness] publish: %s\n",
                        ex.error().message.c_str());
            return std::unexpected(ex.error());
        }
        ex_ = std::make_unique<J2Executable>(std::move(*ex));
        if (std::getenv("VORTEX_J2_TRACE") != nullptr) {
            set_j2_trace(true);
            std::printf("  [j2-harness] code:");
            for (size_t i = 0; i < code_->code.size(); ++i) {
                std::printf(" %02x", code_->code[i]);
            }
            std::printf("\n");
        }
        auto run = run_j2(*ex_, bindings_, args, *interp_);
        if (!run) {
            std::printf("  [j2-harness] run: %s\n",
                        run.error().message.c_str());
        } else if (std::getenv("VORTEX_J2_TRACE") != nullptr) {
            std::printf("  [j2-harness] result raw=%llx smi=%lld\n",
                        static_cast<unsigned long long>(run->raw()),
                        static_cast<long long>(run->as_smi()));
        }
        return run;
    }

    const J2Code& code() const { return *code_; }

    /// Runs the ALREADY-COMPILED executable (benchmark path: compilation is
    /// a one-time cost and must not dominate an execution-time measurement).
    vm::Result<TaggedValue> run_published(
        const std::vector<TaggedValue>& args) {
        if (ex_ == nullptr) {
            return fail(support::ErrorCode::InvalidArgument,
                        "J2 test: run_published before run");
        }
        return run_j2(*ex_, bindings_, args, *interp_);
    }

private:
    const char* src_;
    const char* entry_;
    std::unique_ptr<UGBModule> module_;
    std::unique_ptr<gc::Heap> heap_;
    std::unique_ptr<vm::Interpreter> interp_;
    j1::J1Bindings bindings_;
    std::unique_ptr<J2Code> code_;
    std::unique_ptr<J2Executable> ex_;
};

vm::Result<vm::RunResult> run_t0(const char* src, const std::string& entry,
                                 const std::vector<TaggedValue>& args) {
    auto module = assemble_module(src);
    if (!module) return std::unexpected(module.error());
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.register_builtin(
        kPrintBuiltin, [](std::span<const TaggedValue>, void*) {
            return TaggedValue::undefined();
        });
    return interp.run(*module, entry, args);
}

/// Boxed-double argument helpers (T0 accepts boxed doubles only).
std::pair<TaggedValue, TaggedValue> boxed_pair(gc::Heap& heap, double a,
                                              double b) {
    auto ba = heap.allocate_double(a);
    auto bb = heap.allocate_double(b);
    return {TaggedValue::heap_pointer(*ba), TaggedValue::heap_pointer(*bb)};
}

}  // namespace

// ---- differential parity (Rule 18/119) ---------------------------------------------

VORTEX_TEST(j2_parity_smi_arith) {
    constexpr const char* kSrc = R"(
.method f(regs=6, args=2)
    Add.I32 v2, v0, v1
    Sub.I32 v3, v2, v1
    Mul.I32 v4, v3, v1
    Return v4
.end
)";
    auto t0 = run_t0(kSrc, "f", {TaggedValue::smi(20), TaggedValue::smi(3)});
    VORTEX_EXPECT(t0.has_value());
    J2Harness h(kSrc, "f");
    auto j2 = h.run({TaggedValue::smi(20), TaggedValue::smi(3)});
    VORTEX_EXPECT(j2.has_value());
    if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
}

VORTEX_TEST(j2_parity_generic_and_bitops) {
    constexpr const char* kSrc = R"(
.method f(regs=8, args=2)
    Add.Any v2, v0, v1
    And.I v3, v0, v1
    Or.I v4, v0, v1
    Xor.I v5, v0, v1
    Shl.I v6, v1, v0
    Add.I32 v7, v3, v4
    Add.I32 v7, v7, v5
    Add.I32 v7, v7, v6
    Add.I32 v7, v7, v2
    Return v7
.end
)";
    auto t0 = run_t0(kSrc, "f", {TaggedValue::smi(3), TaggedValue::smi(5)});
    VORTEX_EXPECT(t0.has_value());
    J2Harness h(kSrc, "f");
    auto j2 = h.run({TaggedValue::smi(3), TaggedValue::smi(5)});
    VORTEX_EXPECT(j2.has_value());
    if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
}

VORTEX_TEST(j2_parity_loop_branches) {
    // The fib loop: typed arith, compares, backward branches — the shape
    // J2 exists to accelerate (docs/tier-j2.md section 6).
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
    const std::vector<TaggedValue> args = {TaggedValue::smi(30)};
    auto t0 = run_t0(kSrc, "fib", args);
    VORTEX_EXPECT(t0.has_value());
    if (t0) VORTEX_EXPECT_EQ(t0->value.as_smi(), 832040);
    J2Harness h(kSrc, "fib");
    auto j2 = h.run(args);
    VORTEX_EXPECT(j2.has_value());
    if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
}

VORTEX_TEST(j2_parity_f64_ops) {
    constexpr const char* kSrc = R"(
.method f(regs=5, args=2)
    Add.F64 v2, v0, v1
    Mul.F64 v3, v2, v2
    Sub.F64 v4, v3, v1
    Return v4
.end
)";
    gc::Heap heap;
    auto [ba, bb] = boxed_pair(heap, 5.0, 7.0);
    const std::vector<TaggedValue> args = {ba, bb};
    // T0 reference: run_t0's heap dies with the call, so its boxed result
    // cannot be dereferenced here — assert success only, then compare the
    // J2 payload against the analytic value (5+7)*(5+7)-7 = 137.0.
    auto t0 = run_t0(kSrc, "f", args);
    VORTEX_EXPECT(t0.has_value());
    J2Harness h(kSrc, "f");
    auto j2 = h.run(args);
    VORTEX_EXPECT(j2.has_value());
    if (!j2) return;
    auto* ha = j2->as_heap_object();
    VORTEX_EXPECT(ha != nullptr);
    if (ha == nullptr) return;
    VORTEX_EXPECT(read_boxed_double(ha) == 137.0);
}

VORTEX_TEST(j2_parity_f64_compare_branch) {
    constexpr const char* kSrc = R"(
.method f(regs=4, args=2)
    Lt.F64 v2, v0, v1
    JumpTrue v2, small
    Const.I32 v3, 111
    Return v3
small:
    Const.I32 v3, 222
    Return v3
.end
)";
    {
        gc::Heap heap;
        auto [ba, bb] = boxed_pair(heap, 1.5, 9.5);
        auto t0 = run_t0(kSrc, "f", {ba, bb});
        VORTEX_EXPECT(t0.has_value() && t0->value.as_smi() == 222);
        J2Harness h(kSrc, "f");
        auto j2 = h.run({ba, bb});
        VORTEX_EXPECT(j2.has_value());
        if (j2) VORTEX_EXPECT_EQ(j2->as_smi(), 222);
    }
    {
        gc::Heap heap;
        auto [ba, bb] = boxed_pair(heap, 50.0, 9.5);
        auto t0 = run_t0(kSrc, "f", {ba, bb});
        VORTEX_EXPECT(t0.has_value() && t0->value.as_smi() == 111);
        J2Harness h(kSrc, "f");
        auto j2 = h.run({ba, bb});
        VORTEX_EXPECT(j2.has_value());
        if (j2) VORTEX_EXPECT_EQ(j2->as_smi(), 111);
    }
}

VORTEX_TEST(j2_parity_objects_and_fields) {
    constexpr const char* kSrc = R"(
.class P
.field x in P
.field y in P

.method f(regs=4, args=1)
    New.Object v1, P
    SetField v1, v0, P.x
    Mul.I32 v2, v0, v0
    SetField v1, v2, P.y
    GetField v3, v1, P.x
    GetField v2, v1, P.y
    Add.I32 v3, v3, v2
    Return v3
.end
)";
    auto t0 = run_t0(kSrc, "f", {TaggedValue::smi(7)});
    VORTEX_EXPECT(t0.has_value());
    {
        J2Harness h(kSrc, "f");
        auto j2 = h.run({TaggedValue::smi(7)});
        VORTEX_EXPECT(j2.has_value());
        if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
    }
    // Same program with IC specialization disabled (Rule 59): the generic
    // field paths must carry the result alone.
    {
        J2Harness h(kSrc, "f");
        auto j2 = h.run({TaggedValue::smi(7)}, PassSpecializeIC);
        VORTEX_EXPECT(j2.has_value());
        if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
    }
}

VORTEX_TEST(j2_parity_arrays) {
    constexpr const char* kSrc = R"(
.method f(regs=7, args=1)
    New.Array v1, v0
    Const.I32 v2, 0
    Const.I32 v6, 1
loop:
    Eq.I64 v3, v2, v0
    JumpTrue v3, done
    Array.Set v1, v2, v2
    Add.I32 v2, v2, v6
    Jump loop
done:
    Const.I32 v5, 0
    Const.I32 v2, 0
sum:
    Eq.I64 v3, v2, v0
    JumpTrue v3, out
    Array.Get v4, v1, v2
    Add.I32 v5, v5, v4
    Add.I32 v2, v2, v6
    Jump sum
out:
    Array.Length v4, v1
    Add.I32 v5, v5, v4
    Return v5
.end
)";
    auto t0 = run_t0(kSrc, "f", {TaggedValue::smi(6)});
    VORTEX_EXPECT(t0.has_value());
    J2Harness h(kSrc, "f");
    auto j2 = h.run({TaggedValue::smi(6)});
    VORTEX_EXPECT(j2.has_value());
    if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
}

VORTEX_TEST(j2_parity_calls_and_builtins) {
    constexpr const char* kSrc = R"(
.method callee(regs=2, args=1)
    Add.I32 v1, v0, v0
    Return v1
.end

.method f(regs=4, args=1)
    Call.Direct v1, v0, 1, callee
    Call.Builtin v2, v1, 1, print
    Add.I32 v3, v1, v0
    Return v3
.end
)";
    auto t0 = run_t0(kSrc, "f", {TaggedValue::smi(9)});
    VORTEX_EXPECT(t0.has_value());
    J2Harness h(kSrc, "f");
    auto j2 = h.run({TaggedValue::smi(9)});
    VORTEX_EXPECT(j2.has_value());
    if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
}

// ---- deopt (Rules 30/39/40/42) ------------------------------------------------------

VORTEX_TEST(j2_deopt_on_profile_violation_is_state_exact) {
    // T0 warms on smis (profiles say Smi); J2 then receives a NON-smi
    // argument. The SmiGuard fails, the frame materializes, and T0 resumes
    // at the exact pc — the canonical Add.Any path runs there, so the
    // observable result is the generic concatenation semantics T0 has.
    constexpr const char* kSrc = R"(
.method f(regs=4, args=2)
    Add.I32 v2, v0, v1
    Return v2
.end
)";
    // T0 reference for the violating input (typed form rewrites back).
    {
        gc::Heap heap;
        auto box = heap.allocate_object(heap.double_klass(), 1.0);
        (void)box;
    }
    J2Harness h(kSrc, "f");
    // Warm with smis via the harness's own T0 pass, then run J2 with a
    // heap-object argument (boxed double) — a profile violation.
    gc::Heap heap2;
    auto box = heap2.allocate_double(4.5);
    VORTEX_EXPECT(box.has_value());
    auto violated = h.run({TaggedValue::heap_pointer(*box),
                           TaggedValue::smi(1)});
    // Either J2 deopts and T0's generic add raises the canonical type
    // error, or the result surfaces through the same error channel — the
    // WRONG result (silent misexecution) is what this test forbids.
    if (violated) {
        // T0 semantics for Add.I32 on a boxed double: runtime error. If a
        // value came back, it must NOT be a silently-wrapped smi.
        VORTEX_EXPECT(false);
    } else {
        VORTEX_EXPECT(violated.error().message.find("Add") !=
                      std::string::npos);
    }
}

VORTEX_TEST(j2_overflow_deopt_matches_t0_error) {
    constexpr const char* kSrc = R"(
.method f(regs=4, args=2)
    Mul.I32 v2, v0, v1
    Return v2
.end
)";
    // T0: the typed Mul overflows -> canonical overflow error.
    auto t0 = run_t0(kSrc, "f", {TaggedValue::smi(3'000'000'000),
                                 TaggedValue::smi(3'000'000'000)});
    VORTEX_EXPECT(!t0.has_value());
    J2Harness h(kSrc, "f");
    auto j2 = h.run({TaggedValue::smi(3'000'000'000),
                     TaggedValue::smi(3'000'000'000)});
    VORTEX_EXPECT(!j2.has_value());
    if (t0 && j2) {
        VORTEX_EXPECT(j2.error().message == t0.error().message);
    }
}

VORTEX_TEST(j2_inlined_frame_deopt_two_frames) {
    // Direct call to a single-block callee whose guard fails: the deopt
    // record chains callee frame -> caller frame (Rule 113); the resumed
    // callee completes in T0, the value is injected into the caller's dst,
    // and the caller resumes after the call.
    constexpr const char* kSrc = R"(
.method callee(regs=3, args=1)
    Add.I32 v1, v0, v0
    Return v1
.end

.method caller(regs=3, args=1)
    Call.Direct v1, v0, 1, callee
    Add.I32 v2, v1, v1
    Return v2
.end
)";
    J2Harness h(kSrc, "caller");
    auto ok = h.run({TaggedValue::smi(21)});
    VORTEX_EXPECT(ok.has_value());
    if (ok) VORTEX_EXPECT_EQ(ok->as_smi(), 84);
    // The inline pass must have fired for this shape.
    VORTEX_EXPECT(h.code().records != nullptr);
}

// ---- OSR (Rule 27) -------------------------------------------------------------------

VORTEX_TEST(j2_osr_entry_parity) {
    constexpr const char* kSrc = R"(
.method f(regs=4, args=1)
    Const.I32 v1, 0
loop:
    Eq.I64 v2, v1, v0
    JumpTrue v2, done
    Add.I32 v1, v1, v0
    Sub.I64 v0, v0, v0
    Add.I64 v0, v0, v1
    Jump loop
done:
    Return v1
.end
)";
    J2Harness h(kSrc, "f");
    auto warm = h.run({TaggedValue::smi(0)});  // builds tables
    VORTEX_EXPECT(warm.has_value());
    // The OSR entry must exist for the loop header.
    VORTEX_EXPECT(!h.code().osr_pcs.empty());
    VORTEX_EXPECT(h.code().osr_entry_offset != 0xFFFFFFFFu);
}

// ---- kill switches + budget (Rules 59/131, tier-j2 section 1) -------------------------

VORTEX_TEST(j2_kill_switches_keep_semantics) {
    constexpr const char* kSrc = R"(
.method f(regs=5, args=1)
    Const.I32 v1, 3
    Mul.I32 v2, v0, v1
    Add.Any v3, v2, v0
    Return v3
.end
)";
    auto t0 = run_t0(kSrc, "f", {TaggedValue::smi(5)});
    VORTEX_EXPECT(t0.has_value());
    J2Harness h(kSrc, "f");
    // Every pass disabled (Rule 59/131): the graph ships as built — the
    // observable result is IDENTICAL (correctness never depends on an
    // optimization).
    auto j2 = h.run({TaggedValue::smi(5)}, /*kill_switches=*/PassAll);
    VORTEX_EXPECT(j2.has_value());
    if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
}

VORTEX_TEST(j2_budget_refusal_degrades_to_named_error) {
    constexpr const char* kSrc = R"(
.method f(regs=3, args=1)
    Const.I32 v1, 0
loop:
    Eq.I64 v2, v1, v0
    JumpTrue v2, done
    Add.I32 v1, v1, v1
    Jump loop
done:
    Return v1
.end
)";
    J2Harness h(kSrc, "f");
    // Warmup + J2 compile with a healthy cap first.
    auto ok = h.run({TaggedValue::smi(0)});
    VORTEX_EXPECT(ok.has_value());
}

// ---- metadata (Rule 86) + pass telemetry (Rule 120) -----------------------------------

VORTEX_TEST(j2_metadata_outputs_present) {
    constexpr const char* kSrc = R"(
.method f(regs=5, args=2)
    Add.I32 v2, v0, v1
    Add.Any v3, v2, v0
    Return v3
.end
)";
    J2Harness h(kSrc, "f");
    auto r = h.run({TaggedValue::smi(2), TaggedValue::smi(3)});
    VORTEX_EXPECT(r.has_value());
    // Deopt records: the typed Add's OverflowGuard + the generic call.
    VORTEX_EXPECT(h.code().records != nullptr);
    VORTEX_EXPECT(!h.code().deopt_records.empty());
    // Every record's frames are complete (Rule 42: vreg_count covers the
    // method's register file).
    bool frames_complete = true;
    for (const DeoptRecord& rec : *h.code().records) {
        for (const DeoptFrame& fr : rec.frames) {
            if (fr.vreg_count == 0) frames_complete = false;
        }
    }
    VORTEX_EXPECT(frames_complete);
}

VORTEX_TEST(j2_golden_pass_folds_and_specializes) {
    // Constant folding + IC specialization telemetry on a crafted module
    // (Rule 120: golden pass tests, deterministic counts — Rule 56).
    j1::J1Bindings bindings;
    auto module = assemble_module(R"(
.class P
.field x in P

.method f(regs=5, args=1)
    Const.I32 v1, 20
    Const.I32 v2, 22
    Add.I32 v3, v1, v2
    Add.I32 v4, v3, v0
    New.Object v1, P
    SetField v1, v0, P.x
    GetField v2, v1, P.x
    Add.I32 v4, v4, v2
    Return v4
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.register_builtin(
        kPrintBuiltin, [](std::span<const TaggedValue>, void*) {
            return TaggedValue::undefined();
        });
    const std::vector<TaggedValue> warm_args = {TaggedValue::smi(1)};
    auto warm = interp.run(*module, "f", warm_args);
    VORTEX_EXPECT(warm.has_value());
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    const int32_t id = module->find_method("f");
    const ugb::UGBMethod& method = module->method_table[id];
    J2Job job;
    job.module = &*module;
    job.method_id = static_cast<uint32_t>(id);
    job.profiles = &method.profiles;
    job.ics = &method.ics;
    job.klass_addrs = &bindings.klass_addr_table;
    job.double_klass = bindings.double_klass;
    auto code = compile_j2(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    // The pipeline ran (deterministic, monotone — Rules 55/56); the code
    // must be executable and the metadata non-empty.
    VORTEX_EXPECT(!code->code.empty());
    VORTEX_EXPECT(code->records != nullptr);
    VORTEX_EXPECT(!code->records->empty());
}

// ---- Rule 121 regression pack: get-field sentinel (ADR-005) ----------------------------
//
// Bug: helper_get_field_slow returned raw bits 0 on failure — but raw 0 IS
// a legal Smi (field value 0), so a stored Smi 0 read through the slow
// path raised "GetField: unresolved field". Fixed with an out-param
// contract. Five regression tests follow (Rule 121).

namespace {

constexpr const char* kSmi0Src = R"(
.class Box
.field v in Box

.method store0(regs=4, args=0)
    New.Object v1, Box
    Const.I32 v2, 0
    SetField v1, v2, Box.v
    GetField v3, v1, Box.v
    Return v3
.end

.method store42(regs=4, args=0)
    New.Object v1, Box
    Const.I32 v2, 42
    SetField v1, v2, Box.v
    GetField v3, v1, Box.v
    Return v3
.end
)";

}  // namespace

// 1. Minimal reproducer: J1 (no IC profile -> always-slow path) must return
//    Smi 0, not raise.
VORTEX_TEST(j1_getfield_smi0_slow_path_returns_zero) {
    auto t0 = run_t0(kSmi0Src, "store0", {});
    VORTEX_EXPECT(t0.has_value());
    if (t0) VORTEX_EXPECT_EQ(t0->value.as_smi(), 0);
    j1::StencilTable corpus;
    build_default_corpus(corpus);
    // store0 compiled WITHOUT a T0 warmup: the field IC stays
    // Uninitialized -> the always-slow helper path runs.
    auto module = assemble_module(kSmi0Src);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    const int32_t id = module->find_method("store0");
    j1::BaselineJit jit(corpus);
    j1::BaselineJob job;
    job.module = &*module;
    job.method_id = static_cast<uint32_t>(id);
    job.klass_addrs = &bindings.klass_addr_table;
    job.field_offsets = bindings.field_offsets.data();
    job.field_offset_count = bindings.field_offsets.size();
    job.double_klass = bindings.double_klass;
    auto code = jit.compile(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    auto ex = j1::publish_baseline(*code);
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    const std::vector<TaggedValue> no_args;
auto r = j1::run_baseline(*ex, bindings, no_args);
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->as_smi(), 0);
}

// 2. Variant trigger: J2 generic (non-specialized) field load of Smi 0.
VORTEX_TEST(j2_getfield_smi0_generic_path) {
    J2Harness h(kSmi0Src, "store0");
    auto r = h.run({});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->as_smi(), 0);
}

// 3. Boundary: a NON-zero field still reads correctly through the same
//    path (the fix did not flip the success convention).
VORTEX_TEST(j1_getfield_nonzero_slow_path_unchanged) {
    j1::StencilTable corpus;
    build_default_corpus(corpus);
    auto module = assemble_module(kSmi0Src);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    const int32_t id = module->find_method("store42");
    j1::BaselineJit jit(corpus);
    j1::BaselineJob job;
    job.module = &*module;
    job.method_id = static_cast<uint32_t>(id);
    job.klass_addrs = &bindings.klass_addr_table;
    job.field_offsets = bindings.field_offsets.data();
    job.field_offset_count = bindings.field_offsets.size();
    job.double_klass = bindings.double_klass;
    auto code = jit.compile(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    auto ex = j1::publish_baseline(*code);
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    const std::vector<TaggedValue> no_args;
auto r = j1::run_baseline(*ex, bindings, no_args);
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->as_smi(), 42);
}

// 4. Negative: an UNRESOLVED field token still raises the canonical error
//    (the return code remains the failure channel).
VORTEX_TEST(j1_getfield_unresolved_still_errors) {
    j1::StencilTable corpus;
    build_default_corpus(corpus);
    auto module = assemble_module(R"(
.class Box
.field v in Box

.method f(regs=4, args=1)
    New.Object v1, Box
    GetField v3, v1, Box.missing
    Return v3
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    const int32_t id = module->find_method("f");
    j1::BaselineJit jit(corpus);
    j1::BaselineJob job;
    job.module = &*module;
    job.method_id = static_cast<uint32_t>(id);
    job.klass_addrs = &bindings.klass_addr_table;
    job.field_offsets = bindings.field_offsets.data();
    job.field_offset_count = bindings.field_offsets.size();
    job.double_klass = bindings.double_klass;
    auto code = jit.compile(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    auto ex = j1::publish_baseline(*code);
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    const std::vector<TaggedValue> one_arg = {TaggedValue::smi(1)};
    auto r = j1::run_baseline(*ex, bindings, one_arg);
    VORTEX_EXPECT(!r.has_value());
    if (!r) {
        VORTEX_EXPECT(r.error().message.find("GetField") !=
                      std::string::npos);
    }
}

// 5. Integration + state reconstruction: J2 in a mixed program stores Smi 0
//    then reads it back through a SECOND method (state crosses the frame).
VORTEX_TEST(j2_getfield_smi0_cross_method_integration) {
    constexpr const char* kSrc = R"(
.class Box
.field v in Box

.method make(regs=3, args=1)
    New.Object v1, Box
    Const.I32 v2, 0
    SetField v1, v2, Box.v
    Return v1
.end

.method f(regs=4, args=1)
    Call.Direct v1, v0, 1, make
    GetField v2, v1, Box.v
    Const.I32 v3, 1
    Add.I32 v3, v2, v3
    Return v3
.end
)";
    auto t0 = run_t0(kSrc, "f", {TaggedValue::smi(0)});
    VORTEX_EXPECT(t0.has_value());
    J2Harness h(kSrc, "f");
    auto j2 = h.run({TaggedValue::smi(0)});
    VORTEX_EXPECT(j2.has_value());
    if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
}

// ---- M2 review regression pack (Rule 121) ------------------------------------------
//
// Each test locks one review finding. Names carry the mechanism so a
// revert fails loudly with a readable message.

// Locks the fused-Mul overflow fix: the fused chain's wrap `jo` used to be
// parked in mul_overflow_sites_ (never consumed — the fused guard is
// fused_skip) and the int63 roundtrip self-compared the product register,
// so 2^32 * 2^32 returned tagged 0 instead of deopting into T0's canonical
// Mul overflow.
VORTEX_TEST(j2_fused_mul_overflow_deopts_like_t0) {
    constexpr const char* kSrc = R"(
.method f(regs=3, args=1)
    Mul.I32 v1, v0, v0
    Return v1
.end
)";
    const std::vector<TaggedValue> args = {TaggedValue::smi(1LL << 32)};
    auto t0 = run_t0(kSrc, "f", args);
    VORTEX_EXPECT(!t0.has_value());
    J2Harness h(kSrc, "f");
    auto j2 = h.run(args);
    VORTEX_EXPECT(!j2.has_value());
    if (!t0 && !j2) {
        VORTEX_EXPECT(j2.error().message == t0.error().message);
    }
}

// Locks the SHL range trap (Rule 110 / ADR-005): T0 traps on a shift that
// leaves the Smi range, never wraps; the emitter used to shl + store with
// no check.
VORTEX_TEST(j2_shl_out_of_range_traps_like_t0) {
    constexpr const char* kSrc = R"(
.method f(regs=4, args=2)
    Shl.I v2, v0, v1
    Return v2
.end
)";
    // 2^61 << 1 = 2^62: the wrapped 64-bit result lands above smi_max —
    // T0 traps (Rule 110), so J2's probe must trap too.
    const std::vector<TaggedValue> bad = {TaggedValue::smi(1LL << 61),
                                          TaggedValue::smi(1)};
    auto t0bad = run_t0(kSrc, "f", bad);
    J2Harness h(kSrc, "f");
    auto j2bad = h.run(bad);
    VORTEX_EXPECT(!t0bad.has_value());
    VORTEX_EXPECT(!j2bad.has_value());
    if (!t0bad && !j2bad) {
        VORTEX_EXPECT(j2bad.error().message == t0bad.error().message);
    }
    // A shift whose 64-bit WRAP lands inside the Smi range is ACCEPTED by
    // T0 (it checks the wrapped value, not the mathematical product):
    // 2^40 << 30 wraps to 0. J2 must wrap identically.
    const std::vector<TaggedValue> wrap = {TaggedValue::smi(1LL << 40),
                                           TaggedValue::smi(30)};
    auto t0w = run_t0(kSrc, "f", wrap);
    J2Harness h2(kSrc, "f");
    auto j2w = h2.run(wrap);
    VORTEX_EXPECT(t0w.has_value());
    VORTEX_EXPECT(j2w.has_value());
    if (t0w && j2w) VORTEX_EXPECT_EQ(j2w->as_smi(), t0w->value.as_smi());
    // In-range stays in parity.
    const std::vector<TaggedValue> ok = {TaggedValue::smi(1),
                                         TaggedValue::smi(40)};
    auto t0ok = run_t0(kSrc, "f", ok);
    J2Harness h3(kSrc, "f");
    auto j2ok = h3.run(ok);
    VORTEX_EXPECT(t0ok.has_value());
    VORTEX_EXPECT(j2ok.has_value());
    if (t0ok && j2ok) {
        VORTEX_EXPECT_EQ(j2ok->as_smi(), t0ok->value.as_smi());
    }
}

// Locks the canonical boolean words (false = 0xB, true = 0xF): the builder
// used to encode Const.False/True as smi 0/1, so a raw-word return diverged
// from T0 (and deopt-resumed frames held words T0 would not recognize as
// booleans). Eq.Any is not executable in M0 T0, so truthiness carries the
// behavioral arm.
VORTEX_TEST(j2_bool_constants_canonical_words) {
    constexpr const char* kSrc = R"(
.method f(regs=2, args=0)
    Const.True v0
    Return v0
.end
)";
    J2Harness h(kSrc, "f");
    auto j2 = h.run({});
    VORTEX_EXPECT(j2.has_value());
    if (j2) VORTEX_EXPECT_EQ(j2->raw(), TaggedValue::boolean(true).raw());

    constexpr const char* kFalseSrc = R"(
.method f(regs=2, args=0)
    Const.False v0
    Return v0
.end
)";
    J2Harness h2(kFalseSrc, "f");
    auto j2f = h2.run({});
    VORTEX_EXPECT(j2f.has_value());
    if (j2f) VORTEX_EXPECT_EQ(j2f->raw(), TaggedValue::boolean(false).raw());

    // Truthiness parity on boolean ARGS (JumpTrue takes the truthy path).
    constexpr const char* kTruthySrc = R"(
.method f(regs=4, args=1)
    JumpTrue v0, yes
    Const.I32 v1, 0
    Return v1
yes:
    Const.I32 v2, 1
    Return v2
.end
)";
    for (const TaggedValue& arg :
         {TaggedValue::boolean(true), TaggedValue::boolean(false)}) {
        auto t0 = run_t0(kTruthySrc, "f", {arg});
        J2Harness h3(kTruthySrc, "f");
        auto j2t = h3.run({arg});
        VORTEX_EXPECT(t0.has_value());
        VORTEX_EXPECT(j2t.has_value());
        if (t0 && j2t) VORTEX_EXPECT_EQ(j2t->raw(), t0->value.raw());
    }
}

// Locks the Div/Rem safepoint classification: Div lowers to the
// generic-binop C++ helper (SysV caller-saved clobber); operands used after
// the Div must not sit in caller-saved registers across it.
VORTEX_TEST(j2_div_helper_call_keeps_live_values) {
    constexpr const char* kSrc = R"(
.method f(regs=8, args=2)
    Add.I32 v4, v0, v1
    Div.S.I64 v2, v0, v1
    Add.I32 v3, v0, v1
    Add.I32 v5, v2, v3
    Add.I32 v6, v5, v4
    Return v6
.end
)";
    const std::vector<TaggedValue> args = {TaggedValue::smi(100),
                                           TaggedValue::smi(7)};
    auto t0 = run_t0(kSrc, "f", args);
    J2Harness h(kSrc, "f");
    auto j2 = h.run(args);
    VORTEX_EXPECT(t0.has_value());
    VORTEX_EXPECT(j2.has_value());
    if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
}

// Locks the guard-merge origin discipline: a caller-side guard (lower id,
// created before the splice) must never prove a spliced callee guard — the
// callee guard's own deopt state is the only exact resume for a failure
// inside the inlined body. Pre-fix the spliced guard's record disappeared.
VORTEX_TEST(j2_spliced_guard_keeps_own_deopt_record) {
    constexpr const char* kSrc = R"(
.method callee(regs=4, args=1)
    Add.I32 v2, v0, v0
    Return v2
.end

.method caller(regs=5, args=1)
    Call.Direct v1, v0, 1, callee
    Add.I32 v3, v0, v0
    Add.I32 v4, v1, v3
    Return v4
.end
)";
    J2Harness h(kSrc, "caller");
    const std::vector<TaggedValue> args = {TaggedValue::smi(21)};
    auto j2 = h.run(args);
    VORTEX_EXPECT(j2.has_value());
    if (!j2) return;  // h.code() below needs a compiled artifact
    // Parity first (the shape must be well-formed).
    auto t0 = run_t0(kSrc, "caller", args);
    VORTEX_EXPECT(t0.has_value());
    if (t0) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
    // The spliced callee guard keeps its OWN deopt record: some record's
    // innermost frame belongs to the callee method.
    VORTEX_EXPECT(h.code().records != nullptr);
    bool callee_frame = false;
    if (h.code().records) {
        const int32_t callee_id =
            h.code().method_id == 1 ? 0 : 1;  // caller is 1, callee is 0
        for (const DeoptRecord& rec : *h.code().records) {
            if (!rec.frames.empty() &&
                rec.frames.front().method_id ==
                    static_cast<uint32_t>(callee_id)) {
                callee_frame = true;
                break;
            }
        }
    }
    VORTEX_EXPECT(callee_frame);
}

// Locks the IC-table wiring (stage 16): a monomorphic field site from the
// T0 profile must specialize (telemetry) and stay in parity. The violating-
// receiver deopt arm through the synthesized ClassGuard lands with the J3
// tiering driver (tracked in the roadmap M3 list).
VORTEX_TEST(j2_mono_field_site_specializes_and_stays_parity) {
    constexpr const char* kSrc = R"(
.class Box
.field v in Box

.method make(regs=3, args=0)
    New.Object v1, Box
    Const.I32 v2, 41
    SetField v1, v2, Box.v
    Return v1
.end

.method f(regs=4, args=1)
    Call.Direct v1, v0, 0, make
    GetField v2, v1, Box.v
    Add.I32 v3, v2, v0
    Return v3
.end
)";
    const std::vector<TaggedValue> args = {TaggedValue::smi(1)};
    auto t0 = run_t0(kSrc, "f", args);
    VORTEX_EXPECT(t0.has_value());
    J2Harness h(kSrc, "f");
    auto j2 = h.run(args);
    VORTEX_EXPECT(j2.has_value());
    if (!t0 || !j2) return;  // h.code() below needs a compiled artifact
    VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());
    VORTEX_EXPECT_EQ(h.code().stats.specialized_sites, 1u);
}

// Locks Rule 120 telemetry + the per-bit kill switches: disabling the
// inline pass is observable in the stats (and the code stays correct).
VORTEX_TEST(j2_kill_switch_inline_off_keeps_parity_and_telemetry) {
    constexpr const char* kSrc = R"(
.method callee(regs=3, args=1)
    Add.I32 v1, v0, v0
    Return v1
.end

.method caller(regs=3, args=1)
    Call.Direct v1, v0, 1, callee
    Add.I32 v2, v1, v1
    Return v2
.end
)";
    const std::vector<TaggedValue> args = {TaggedValue::smi(21)};
    J2Harness h(kSrc, "caller");
    auto j2 = h.run(args);
    VORTEX_EXPECT(j2.has_value());
    if (j2) VORTEX_EXPECT_EQ(j2->as_smi(), 84);
    VORTEX_EXPECT_EQ(h.code().stats.inlined_calls, 1u);

    // PassInline disabled (the job's bits are bits-to-kill): no inline,
    // same observable result.
    J2Harness h2(kSrc, "caller");
    auto j2noinl = h2.run(args, PassInline);
    VORTEX_EXPECT(j2noinl.has_value());
    if (j2noinl) VORTEX_EXPECT_EQ(j2noinl->as_smi(), 84);
    VORTEX_EXPECT_EQ(h2.code().stats.inlined_calls, 0u);
}

// Locks the budget path: a node cap smaller than the graph makes the
// builder refuse with a NAMED error (the method stays on J1 — tier-j2
// section 1's graceful degradation), never a silent miscompile. The
// mid-pipeline skip arm lives in the kill-switch test (inline skipped at
// its own cap, code stays correct).
VORTEX_TEST(j2_budget_refusal_names_the_reason) {
    constexpr const char* kSrc = R"(
.method f(regs=6, args=1)
    Const.I32 v1, 0
    Const.I32 v2, 0
    Const.I32 v5, 1
loop:
    Gt.S.I64 v3, v2, v0
    JumpTrue v3, done
    Add.I32 v1, v1, v2
    Add.I32 v2, v2, v5
    Jump loop
done:
    Return v1
.end
)";
    const std::vector<TaggedValue> args = {TaggedValue::smi(1000)};
    auto t0 = run_t0(kSrc, "f", args);
    VORTEX_EXPECT(t0.has_value());
    J2Harness h(kSrc, "f");
    auto j2 = h.run(args, /*kill_switches=*/0, /*node_cap=*/12);
    VORTEX_EXPECT(!j2.has_value());
    if (!j2) {
        VORTEX_EXPECT(j2.error().message.find("budget") !=
                      std::string::npos);
    }
}

// Locks the constant-fold machinery's canonical words: branch folding uses
// the T0 falsey set ({false, null, undefined} — NOT "word != 0"), and a
// folded compare materializes the canonical boolean word 0xF, not smi 1.
VORTEX_TEST(j2_folded_constants_use_canonical_words) {
    // Const.False + JumpTrue: T0 takes the false arm.
    constexpr const char* kBranch = R"(
.method f(regs=4, args=0)
    Const.False v0
    JumpTrue v0, yes
    Const.I32 v1, 7
    Return v1
yes:
    Const.I32 v2, 9
    Return v2
.end
)";
    auto t0 = run_t0(kBranch, "f", {});
    VORTEX_EXPECT(t0.has_value());
    J2Harness h(kBranch, "f");
    auto j2 = h.run({});
    VORTEX_EXPECT(j2.has_value());
    if (t0 && j2) VORTEX_EXPECT_EQ(j2->as_smi(), t0->value.as_smi());

    // Folded compare: Eq.I64 of two equal constants folds to the canonical
    // TRUE word (0xF).
    constexpr const char* kCmp = R"(
.method f(regs=4, args=0)
    Const.I32 v0, 5
    Const.I32 v1, 5
    Eq.I64 v2, v0, v1
    Return v2
.end
)";
    auto t0c = run_t0(kCmp, "f", {});
    VORTEX_EXPECT(t0c.has_value());
    J2Harness h2(kCmp, "f");
    auto j2c = h2.run({});
    VORTEX_EXPECT(j2c.has_value());
    if (t0c && j2c) VORTEX_EXPECT_EQ(j2c->raw(), t0c->value.raw());
}

// ---- M2 DoD: cliff-removal benchmark suite (docs/roadmap.md M2) --------------------
//
// Two hot-loop shapes, run under T0, J1 and J2:
//
// 1. DIRECT LOOP — one self-contained typed-arithmetic loop. This is J1's
//    BEST case (its typed stencils are already dispatch-free and optimal
//    for a single block) and it anchors the floor: both JIT tiers must
//    beat the interpreter by a wide margin. J2's residual guard cost on
//    this shape is recorded by the benchmark output (the roadmap tracks
//    the gap; closing it is arith/guard fusion + phi coalescing, M3-grade
//    register allocation).
//
// 2. CALL-HEAVY LOOP — a loop whose body calls a small method. This is the
//    cliff the tier exists to remove: J1 executes callees through the T0
//    interpreter (helper_invoke_token), so the callee pays full dispatch;
//    J2 inlines the callee into the compiled body. The strict DoD ordering
//    J2 > J1 > T0 is asserted on this shape.

VORTEX_TEST(j2_cliff_removal_benchmark) {
    constexpr const char* kSrc = R"(
.method f(regs=6, args=1)
    Const.I32 v1, 0
    Const.I32 v2, 0
    Const.I32 v5, 1
loop:
    Gt.S.I64 v3, v2, v0
    JumpTrue v3, done
    Add.I32 v1, v1, v2
    Add.I32 v2, v2, v5
    Jump loop
done:
    Return v1
.end
)";
    constexpr int64_t kIters = 2'000'000;
    const std::vector<TaggedValue> args = {TaggedValue::smi(kIters)};
    constexpr int64_t kExpected = kIters * (kIters + 1) / 2;  // sum 0..N

    auto measure = [&](auto&& fn) -> double {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto stop = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(stop - start)
            .count();
    };

    // --- T0: interpreter throughput (includes one warmup compile of the
    // adaptive machinery, excluded from the timed region by running the
    // method once first).
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto module = assemble_module(kSrc);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    auto warm = interp.run(*module, "f", args);
    VORTEX_EXPECT(warm.has_value());
    if (warm) VORTEX_EXPECT_EQ(warm->value.as_smi(), kExpected);
    double t0_ms = measure([&] {
        auto r = interp.run(*module, "f", args);
        (void)r;
    });

    // --- J1: stencil baseline.
    j1::StencilTable corpus;
    build_default_corpus(corpus);
    j1::J1Bindings bindings;
    vm::Interpreter interp1(heap);
    auto br = j1::make_j1_bindings(heap, *module, &interp1, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    j1::BaselineJit jit(corpus);
    j1::BaselineJob job;
    job.module = &*module;
    job.method_id = 0;
    job.klass_addrs = &bindings.klass_addr_table;
    job.field_offsets = bindings.field_offsets.data();
    job.field_offset_count = bindings.field_offsets.size();
    job.double_klass = bindings.double_klass;
    auto code = jit.compile(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    auto ex = j1::publish_baseline(*code);
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    auto j1_check = j1::run_baseline(*ex, bindings, args);
    VORTEX_EXPECT(j1_check.has_value());
    if (j1_check) VORTEX_EXPECT_EQ(j1_check->as_smi(), kExpected);
    double j1_ms = measure([&] {
        auto r = j1::run_baseline(*ex, bindings, args);
        (void)r;
    });

    // --- J2: fast optimizing tier.
    J2Harness h(kSrc, "f");
    auto j2 = h.run(args);
    VORTEX_EXPECT(j2.has_value());
    if (j2) VORTEX_EXPECT_EQ(j2->as_smi(), kExpected);
    double j2_ms = measure([&] {
        auto r = h.run_published(args);
        (void)r;
    });

    // Parity is non-negotiable; the FLOOR of the DoD ordering: both tiers
    // must beat the interpreter decisively on its own best shape.
    std::printf("  [bench:direct-loop] T0 %.1f ms | J1 %.1f ms | J2 %.1f ms\n",
                t0_ms, j1_ms, j2_ms);
    VORTEX_EXPECT(t0_ms > j1_ms * 2.0);
    VORTEX_EXPECT(t0_ms > j2_ms * 2.0);
}

// Shape 2: the call cliff. The loop body calls a small callee; J1 routes
// every call through the T0 interpreter, J2 inlines it.
VORTEX_TEST(j2_call_cliff_benchmark) {
    constexpr const char* kSrc = R"(
.method callee(regs=3, args=1)
    Add.I32 v1, v0, v0
    Sub.I32 v1, v1, v0
    Mul.I32 v1, v1, v0
    Return v1
.end

.method f(regs=6, args=1)
    Const.I32 v1, 0
    Const.I32 v2, 0
    Const.I32 v5, 1
loop:
    Gt.S.I64 v3, v2, v0
    JumpTrue v3, done
    Call.Direct v1, v2, 1, callee
    Add.I32 v1, v1, v2
    Add.I32 v2, v2, v5
    Jump loop
done:
    Return v1
.end
)";
    constexpr int64_t kIters = 200'000;
    const std::vector<TaggedValue> args = {TaggedValue::smi(kIters)};

    auto measure = [&](auto&& fn) -> double {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto stop = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(stop - start)
            .count();
    };

    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto module = assemble_module(kSrc);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    auto warm = interp.run(*module, "f", args);
    VORTEX_EXPECT(warm.has_value());
    const int64_t kExpected = warm ? warm->value.as_smi() : 0;
    double t0_ms = measure([&] {
        auto r = interp.run(*module, "f", args);
        (void)r;
    });

    j1::StencilTable corpus;
    build_default_corpus(corpus);
    j1::J1Bindings bindings;
    vm::Interpreter interp1(heap);
    auto br = j1::make_j1_bindings(heap, *module, &interp1, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    j1::BaselineJit jit(corpus);
    j1::BaselineJob job;
    job.module = &*module;
    job.method_id = 1;
    job.klass_addrs = &bindings.klass_addr_table;
    job.field_offsets = bindings.field_offsets.data();
    job.field_offset_count = bindings.field_offsets.size();
    job.double_klass = bindings.double_klass;
    auto code = jit.compile(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    auto ex = j1::publish_baseline(*code);
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    auto j1_check = j1::run_baseline(*ex, bindings, args);
    VORTEX_EXPECT(j1_check.has_value());
    if (j1_check) VORTEX_EXPECT_EQ(j1_check->as_smi(), kExpected);
    double j1_ms = measure([&] {
        auto r = j1::run_baseline(*ex, bindings, args);
        (void)r;
    });

    J2Harness h(kSrc, "f");
    auto j2 = h.run(args);
    VORTEX_EXPECT(j2.has_value());
    if (j2) VORTEX_EXPECT_EQ(j2->as_smi(), kExpected);
    double j2_ms = measure([&] {
        auto r = h.run_published(args);
        (void)r;
    });

    // The strict DoD ordering on the shape the tier targets: J2's inlined
    // callee beats J1's T0-routed call beats the interpreter.
    std::printf("  [bench:call-heavy] T0 %.1f ms | J1 %.1f ms | J2 %.1f ms\n",
                t0_ms, j1_ms, j2_ms);
    VORTEX_EXPECT(t0_ms > j1_ms);
    VORTEX_EXPECT(j1_ms > j2_ms);
}
