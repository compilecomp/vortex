// T0 interpreter end-to-end tests: arithmetic, branches, calls, ICs,
// adaptive rewriting, allocation, GC barriers.
#include "vortex_test.hpp"

#include "vortex/gc/icggc.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/verifier.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;
using namespace vortex::ugb;

namespace {

vm::Result<vm::RunResult> run_source(const char* src, const std::string& entry,
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

}  // namespace

VORTEX_TEST(t0_arithmetic_add) {
    auto r = run_source(R"(
.method f(regs=4, args=2)
    ADD_ANY v2, v0, v1
    Return v2
.end
)", "f", {TaggedValue::smi(20), TaggedValue::smi(22)});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 42);
}

VORTEX_TEST(t0_adaptive_rewrite_canonical_to_typed) {
    auto module = assemble_module(R"(
.method f(regs=3, args=0)
    Const.I32 v0, 2
    Const.I32 v1, 3
loop:
    ADD_ANY v2, v0, v1
    Jump exit
exit:
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;

    gc::Heap heap;
    vm::Interpreter interp(heap);
    vm::InterpreterConfig cfg;
    cfg.typed_rewrite_threshold = 4;
    interp.config_mutable() = cfg;

    auto r = interp.run(*module, "f", {});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 5);

    // To force the rewrite we simply call the same method many times.
    for (int i = 0; i < 10; ++i) {
        auto rr = interp.run(*module, "f", {});
        VORTEX_EXPECT(rr.has_value());
    }
    VORTEX_EXPECT(interp.stats().typed_rewrites > 0);
}

VORTEX_TEST(t0_typed_demotes_on_chronic_failure) {
    // Add.I32 fails on non-integer operands; after the threshold the site is
    // rewritten back to Add.Any.
    auto module = assemble_module(R"(
.method f(regs=3, args=2)
    ADD_I32 v2, v0, v1
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    vm::InterpreterConfig cfg;
    cfg.generic_rewrite_threshold = 2;
    interp.config_mutable() = cfg;
    // Null operands fail the speculation every time.
    const std::vector<TaggedValue> null_args = {TaggedValue::null(), TaggedValue::null()};
    for (int i = 0; i < 3; ++i) {
        auto r = interp.run(*module, "f", null_args);
        VORTEX_EXPECT(!r.has_value());  // canonical path also rejects nulls
    }
    VORTEX_EXPECT(interp.stats().generic_rewrites > 0);
}

VORTEX_TEST(t0_branches_and_loops) {
    // Sum 1..100 => 5050.
    auto r = run_source(R"(
.method sum(regs=4, args=1)
    Const.I32 v1, 0
loop:
    Const.I32 v2, 0
    Eq.I64 v3, v0, v2
    JumpTrue v3, done
    Add.I64 v1, v1, v0
    Const.I32 v2, 1
    Sub.I64 v0, v0, v2
    Jump loop
done:
    Return v1
.end
.method main(regs=3, args=0)
    Const.I32 v0, 100
    Call.Direct v1, v0, 1, sum
    Return v1
.end
)", "main", {});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 5050);
}

VORTEX_TEST(t0_recursion) {
    // Recursive factorial: fact(10) = 3628800.
    auto r = run_source(R"(
.method fact(regs=7, args=1)
    Const.I32 v1, 1
    Le.S.I64 v2, v0, v1
    JumpFalse v2, rec
    Return v1
rec:
    Const.I32 v3, 1
    Sub.I64 v4, v0, v3
    Call.Direct v5, v4, 1, fact
    Mul.I64 v6, v0, v5
    Return v6
.end
.method main(regs=2, args=0)
    Const.I32 v0, 10
    Call.Direct v1, v0, 1, fact
    Return v1
.end
)", "main", {});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 3628800);
}

VORTEX_TEST(t0_field_access) {
    auto r = run_source(R"(
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
)", "main", {});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 7);
}

VORTEX_TEST(t0_inline_cache_hit_miss_counters) {
    // Repeated field access through the same shape fills the monomorphic IC
    // and shows hits (docs/tier-t0.md section 3).
    auto module = assemble_module(R"(
.class Point
.field x in Point

.method make(regs=3, args=1)
    New.Object v1, Point
    SetField v1, v0, Point.x
    Return v1
.end

.method getx(regs=2, args=1)
    GetField v1, v0, Point.x
    Return v1
.end

.method main(regs=3, args=0)
    Const.I32 v0, 5
    Call.Direct v1, v0, 1, make
    Call.Direct v2, v1, 1, getx
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.register_builtin("print", [](std::span<const TaggedValue>, void*) {
        return TaggedValue::undefined();
    });
    for (int i = 0; i < 5; ++i) {
        auto r = interp.run(*module, "main", {});
        VORTEX_EXPECT(r.has_value());
        if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 5);
    }
    // First run misses (cold IC) and records; subsequent runs hit.
    VORTEX_EXPECT(interp.stats().ic_hits > 0);
    VORTEX_EXPECT(interp.stats().ic_misses > 0);
}

VORTEX_TEST(t0_array_roundtrip) {
    auto r = run_source(R"(
.method main(regs=5, args=0)
    Const.I32 v0, 4
    New.Array v1, v0
    Const.I32 v2, 0
    Const.I32 v3, 111
    Array.Set v1, v2, v3
    Array.Get v4, v1, v2
    Return v4
.end
)", "main", {});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 111);
}

VORTEX_TEST(t0_bounds_check_traps) {
    auto r = run_source(R"(
.method main(regs=4, args=0)
    Const.I32 v0, 1
    New.Array v1, v0
    Const.I32 v2, 5
    Const.I32 v3, 9
    Array.Set v1, v2, v3
    Return v3
.end
)", "main", {});
    VORTEX_EXPECT(!r.has_value());
}

VORTEX_TEST(t0_card_marking_on_ref_store) {
    auto module = assemble_module(R"(
.class Node
.field next in Node

.method main(regs=3, args=0)
    New.Object v0, Node
    New.Object v1, Node
    SetField v0, v1, Node.next
    Return v0
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto r = interp.run(*module, "main", {});
    VORTEX_EXPECT(r.has_value());
    // The reference store marked a card (docs/gc-icggc.md section 3).
    VORTEX_EXPECT(heap.card_table().dirty_count() > 0);
}

VORTEX_TEST(t0_gc_maps_and_profiles_ready) {
    auto module = assemble_module(R"(
.method f(regs=3, args=2)
    ADD_ANY v2, v0, v1
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    const std::vector<TaggedValue> args = {TaggedValue::smi(1), TaggedValue::smi(2)};
    auto r = interp.run(*module, "f", args);
    VORTEX_EXPECT(r.has_value());
    VORTEX_EXPECT(module->method_table[0].runtime_tables_ready);
    VORTEX_EXPECT(module->method_table[0].profiles.size() > 0);
    VORTEX_EXPECT_EQ(r->stats.instructions_executed, uint64_t{2});
}
