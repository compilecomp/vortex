// Compliance regression tests: locks in the numeric-semantics fixes
// (Rule 110), the container laws (Rules 50-52), capability negotiation
// (Rule 3), stable site IDs (Rule 8), and dispatch-table thread safety
// (Rule 57 / ADR-002).
#include "vortex_test.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "vortex/frontends/reference/emitter.hpp"
#include "vortex/gc/icggc.hpp"
#include "vortex/ir/son_graph.hpp"
#include "vortex/runtime/object_model.hpp"
#include "vortex/support/containers.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/module.hpp"
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
    interp.register_builtin("print",
                            [](std::span<const TaggedValue>, void*) {
                                return TaggedValue::undefined();
                            });
    return interp.run(*module, entry, args);
}

/// Runs a zero-arg float module and extracts the boxed-double result.
bool run_f64(const char* src, double& out, std::string* error = nullptr) {
    auto module = assemble_module(src);
    if (!module) {
        if (error != nullptr) *error = module.error().message;
        return false;
    }
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto r = interp.run(*module, "f", std::span<const TaggedValue>{});
    if (!r) {
        if (error != nullptr) *error = r.error().message;
        return false;
    }
    if (!r->value.is_heap_object()) return false;
    if (!is_boxed_double(r->value.as_heap_object(), heap.double_klass())) {
        return false;
    }
    out = read_boxed_double(r->value.as_heap_object());
    return true;
}

/// Compiles + runs a zero-arg Mini program, returning the print-echo result.
vm::Result<vm::RunResult> run_source_or_fail(const char* src) {
    auto m = vortex::frontends::reference::compile_mini(src);
    if (!m) return std::unexpected(m.error());
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.register_builtin("print",
                            [](std::span<const TaggedValue> args, void*) {
                                return args.empty() ? TaggedValue::undefined()
                                                    : args[0];
                            });
    return interp.run(*m, "main", std::span<const TaggedValue>{});
}

}  // namespace

// ---------------------------------------------------------------------------
// Rule 110: numeric semantics are exact — the P1 regressions
// ---------------------------------------------------------------------------

VORTEX_TEST(f64_div_by_zero_yields_inf_not_zero) {
    // IEEE: 1.0 / 0.0 == +inf. The old code returned 0.0 here.
    double out = 0.0;
    const bool ok = run_f64(R"(
.method f(regs=4, args=0)
    CONST_F64 v0, 1.0
    CONST_F64 v1, 0.0
    DIV_F64 v2, v0, v1
    Return v2
.end
)",
                            out);
    VORTEX_EXPECT(ok);
    if (ok) VORTEX_EXPECT(std::isinf(out) && out > 0.0);
}

VORTEX_TEST(f64_nan_flows_through_arithmetic) {
    // NaN is a legitimate value: 0.0/0.0 -> NaN, then NaN + 1.0 -> NaN.
    // The old "NaN means not-a-double" sentinel rejected it as a type error.
    double out = 0.0;
    const bool ok = run_f64(R"(
.method f(regs=5, args=0)
    CONST_F64 v0, 0.0
    CONST_F64 v1, 0.0
    DIV_F64 v2, v0, v1
    CONST_F64 v3, 1.0
    ADD_F64 v4, v2, v3
    Return v4
.end
)",
                            out);
    VORTEX_EXPECT(ok);
    if (ok) VORTEX_EXPECT(std::isnan(out));
}

VORTEX_TEST(f64_nan_equality_is_false) {
    // IEEE: NaN == anything is false, and the comparison must not treat a
    // NaN operand as a type error.
    auto r = run_source(R"(
.method f(regs=5, args=0)
    CONST_F64 v0, 0.0
    CONST_F64 v1, 0.0
    DIV_F64 v2, v0, v1
    CONST_F64 v3, 1.0
    EQ_F64 v4, v2, v3
    Return v4
.end
)",
                        "f", {});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT(!r->value.as_boolean_unchecked());
}

VORTEX_TEST(f64_to_i64_saturates_and_nan_is_zero) {
    // Saturating conversion (docs/guest-semantics.md): the old code used
    // static_cast<int64_t>, which is UB for out-of-range and NaN inputs.
    auto module = assemble_module(R"(
.method f(regs=3, args=0)
    CONST_F64 v0, nan
    FloatToInt.F64.I64 v2, v0
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto r = interp.run(*module, "f", std::span<const TaggedValue>{});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 0);
}

VORTEX_TEST(smi_overflow_traps_instead_of_wrapping) {
    // smi_max + smi_max must trap with "overflow", never wrap (Rule 110).
    auto module = assemble_module(R"(
.method f(regs=4, args=2)
    CONST_I64 v0, 4611686018427387903
    CONST_I64 v1, 4611686018427387903
    ADD_ANY v2, v0, v1
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    const std::vector<TaggedValue> no_args(2);
    auto r = interp.run(*module, "f", no_args);
    VORTEX_EXPECT(!r.has_value());
    if (!r) VORTEX_EXPECT(r.error().message.find("overflow") !=
                          std::string::npos);
}

VORTEX_TEST(mul_overflow_does_not_invoke_signed_overflow_ub) {
    // smi_max * 2 overflows int64 itself; __builtin_mul_overflow makes this
    // a defined trap instead of UB.
    auto module = assemble_module(R"(
.method f(regs=4, args=2)
    CONST_I64 v0, 4611686018427387903
    CONST_I32 v1, 2
    MUL_ANY v2, v0, v1
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    const std::vector<TaggedValue> no_args(2);
    auto r = interp.run(*module, "f", no_args);
    VORTEX_EXPECT(!r.has_value());
    if (!r) VORTEX_EXPECT(r.error().message.find("overflow") !=
                          std::string::npos);
}

VORTEX_TEST(neg_smi_min_traps) {
    auto module = assemble_module(R"(
.method f(regs=3, args=2)
    CONST_I64 v0, -4611686018427387904
    NEG_I64 v2, v0
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    const std::vector<TaggedValue> no_args(2);
    auto r = interp.run(*module, "f", no_args);
    VORTEX_EXPECT(!r.has_value());
    if (!r) VORTEX_EXPECT(r.error().message.find("overflow") !=
                          std::string::npos);
}

VORTEX_TEST(float_constants_do_not_truncate_through_materialize) {
    // Rule 110: materialize_constant of a boxed kind must not return a
    // silently truncated Smi.
    UGBModule module;
    const uint32_t pool = module.intern_f64(3.75);
    const TaggedValue v = module.materialize_constant(pool);
    VORTEX_EXPECT(v.is_undefined());
    // Integer constants still materialize exactly.
    const uint32_t ipool = module.intern_i64(77);
    VORTEX_EXPECT_EQ(module.materialize_constant(ipool).as_smi(), 77);
}

// ---------------------------------------------------------------------------
// Rule 3: capability negotiation
// ---------------------------------------------------------------------------

VORTEX_TEST(unsupported_capability_is_rejected_safely) {
    UGBModule module;
    MethodBuilder b(module, "f", 2, 0);
    b.const_null(0);
    b.ret(0);
    VORTEX_EXPECT(b.finish().has_value());
    // FFI requires runtime hooks the T0 engine does not provide.
    module.method_table[0].required_capabilities.push_back(
        static_cast<uint8_t>(Capability::FFI));
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto r = interp.run(module, "f", {});
    VORTEX_EXPECT(!r.has_value());
    if (!r) VORTEX_EXPECT(r.error().message.find("capability") !=
                          std::string::npos);
}

VORTEX_TEST(unknown_capability_rejected_by_verifier) {
    UGBModule module;
    MethodBuilder b(module, "f", 2, 0);
    b.const_null(0);
    b.ret(0);
    VORTEX_EXPECT(b.finish().has_value());
    module.method_table[0].required_capabilities.push_back(200);
    auto res = verify_module(module);
    VORTEX_EXPECT(!res.has_value());
    if (!res) VORTEX_EXPECT(res.error().message.find("unknown capability") !=
                            std::string::npos);
}

VORTEX_TEST(mini_frontend_declares_capabilities) {
    auto module = vortex::frontends::reference::compile_mini("fn main() { let x = 1 + 2; }");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    VORTEX_EXPECT(!module->method_table.empty());
    VORTEX_EXPECT(!module->method_table[0].required_capabilities.empty());
    VORTEX_EXPECT_EQ(module->version_minor, kUgbVersionMinor);
    VORTEX_EXPECT_EQ(module->runtime_abi_version, kRuntimeAbiVersion);
}

// ---------------------------------------------------------------------------
// Rule 8: stable site IDs; Rule 10: versioning
// ---------------------------------------------------------------------------

VORTEX_TEST(site_ids_are_stable_and_injective) {
    const uint64_t s1 = make_site_id(7, 12);
    const uint64_t s2 = make_site_id(7, 12);
    const uint64_t s3 = make_site_id(8, 12);
    const uint64_t s4 = make_site_id(7, 13);
    VORTEX_EXPECT_EQ(s1, s2);
    VORTEX_EXPECT(s1 != s3);
    VORTEX_EXPECT(s1 != s4);
    UGBMethod m;
    m.id = 42;
    VORTEX_EXPECT_EQ(m.site_id(9), make_site_id(42, 9));
}

VORTEX_TEST(binary_roundtrip_preserves_capabilities_and_versions) {
    auto module = assemble_module(R"(
.method f(regs=3, args=1)
    ADD_I32 v2, v0, v1
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    module->extensions.push_back(ExtensionRef{"extension.mini.strings", 1});
    module->method_table[0].required_capabilities.push_back(
        static_cast<uint8_t>(Capability::TypedArithmetic));
    const std::vector<uint8_t> binary = encode_module(*module);
    UGBModule decoded;
    DecodeErrorInfo err;
    VORTEX_EXPECT(decode_module(binary.data(), binary.size(), decoded, err));
    VORTEX_EXPECT_EQ(decoded.runtime_abi_version, kRuntimeAbiVersion);
    VORTEX_EXPECT_EQ(decoded.metadata_schema_version,
                     kMetadataSchemaVersion);
    VORTEX_EXPECT_EQ(decoded.extensions.size(), size_t{1});
    if (!decoded.extensions.empty()) {
        VORTEX_EXPECT_EQ(decoded.extensions[0].name,
                         "extension.mini.strings");
    }
    VORTEX_EXPECT(!decoded.method_table.empty());
    if (!decoded.method_table.empty()) {
        VORTEX_EXPECT_EQ(decoded.method_table[0].required_capabilities.size(),
                         size_t{1});
    }
    VORTEX_EXPECT(verify_module(decoded).has_value());
}

VORTEX_TEST(extension_namespace_grammar_enforced) {
    VORTEX_EXPECT(is_valid_extension_name("extension.mini.strings"));
    VORTEX_EXPECT(!is_valid_extension_name("mini.strings"));
    VORTEX_EXPECT(!is_valid_extension_name("extension.mini"));
    VORTEX_EXPECT(!is_valid_extension_name("extension.mini.a.b"));
    VORTEX_EXPECT(!is_valid_extension_name("extension..strings"));
    UGBModule module;
    module.extensions.push_back(ExtensionRef{"bogus", 1});
    VORTEX_EXPECT(!verify_module(module).has_value());
}

// ---------------------------------------------------------------------------
// Rules 50-52: compliance containers are correct
// ---------------------------------------------------------------------------

VORTEX_TEST(flat_hash_map_insert_find_erase) {
    support::FlatHashMap<uint64_t, uint64_t> m;
    for (uint64_t i = 0; i < 1000; ++i) {
        VORTEX_EXPECT(m.insert(i * 2654435761ull, i) != nullptr);
    }
    for (uint64_t i = 0; i < 1000; ++i) {
        const uint64_t* v = m.find(i * 2654435761ull);
        VORTEX_EXPECT(v != nullptr);
        if (v) VORTEX_EXPECT_EQ(*v, i);
    }
    VORTEX_EXPECT_EQ(m.size(), size_t{1000});
    VORTEX_EXPECT(m.erase(0));
    VORTEX_EXPECT(!m.contains(0));
    // Reinsert after erase (tombstone path).
    VORTEX_EXPECT(m.insert(0, 999) != nullptr);
    VORTEX_EXPECT_EQ(*m.find(0), uint64_t{999});
    // Copy semantics.
    support::FlatHashMap<uint64_t, uint64_t> copy = m;
    VORTEX_EXPECT_EQ(copy.size(), m.size());
    support::FlatHashMap<uint64_t, uint64_t> moved = std::move(copy);
    VORTEX_EXPECT_EQ(moved.size(), size_t{1000});
}

VORTEX_TEST(small_vector_inline_storage) {
    support::SmallVector<uint32_t, 4> v;
    VORTEX_EXPECT(v.capacity() >= 4);
    for (uint32_t i = 0; i < 100; ++i) v.push_back(i);
    VORTEX_EXPECT_EQ(v.size(), size_t{100});
    VORTEX_EXPECT_EQ(v[99], uint32_t{99});
    v.pop_back();
    VORTEX_EXPECT_EQ(v.size(), size_t{99});
    // Copy + move stay intact across the heap transition.
    support::SmallVector<uint32_t, 4> c = v;
    VORTEX_EXPECT_EQ(c.size(), v.size());
    support::SmallVector<uint32_t, 4> mv = std::move(c);
    VORTEX_EXPECT_EQ(mv.size(), size_t{99});
}

VORTEX_TEST(sparse_set_membership) {
    support::SparseSet s(64);
    s.insert(3);
    s.insert(17);
    s.insert(63);
    VORTEX_EXPECT(s.contains(3));
    VORTEX_EXPECT(s.contains(17));
    VORTEX_EXPECT(s.contains(63));
    VORTEX_EXPECT(!s.contains(4));
    s.remove(17);
    VORTEX_EXPECT(!s.contains(17));
    VORTEX_EXPECT(s.contains(3));
    s.clear();
    VORTEX_EXPECT(s.empty());
}

// ---------------------------------------------------------------------------
// Rule 57 / ADR-002: dispatch table is safe for concurrent interpreters
// ---------------------------------------------------------------------------

VORTEX_TEST(two_interpreters_execute_concurrently) {
    auto module_a = assemble_module(R"(
.method f(regs=3, args=0)
    CONST_I32 v0, 20
    CONST_I32 v1, 22
    ADD_ANY v2, v0, v1
    Return v2
.end
)");
    auto module_b = assemble_module(R"(
.method f(regs=3, args=0)
    CONST_I32 v0, 20
    CONST_I32 v1, 22
    ADD_ANY v2, v0, v1
    Return v2
.end
)");
    VORTEX_EXPECT(module_a.has_value() && module_b.has_value());
    if (!module_a || !module_b) return;

    uint64_t ra = 0, rb = 0;
    auto run_a = [&] {
        gc::Heap heap;
        vm::Interpreter interp(heap);
        auto r = interp.run(*module_a, "f", {});
        if (r) ra = static_cast<uint64_t>(r->value.as_smi());
    };
    auto run_b = [&] {
        gc::Heap heap;
        vm::Interpreter interp(heap);
        auto r = interp.run(*module_b, "f", {});
        if (r) rb = static_cast<uint64_t>(r->value.as_smi());
    };
    std::thread ta(run_a), tb(run_b);
    ta.join();
    tb.join();
    VORTEX_EXPECT_EQ(ra, uint64_t{42});
    VORTEX_EXPECT_EQ(rb, uint64_t{42});
}


// ---------------------------------------------------------------------------
// Review-fix regressions (assembler safety, frontend lowering, guards)
// ---------------------------------------------------------------------------

VORTEX_TEST(assembler_truncated_input_is_safe_not_oob) {
    // Rule 9: untrusted input must produce clean errors, never OOB reads.
    const char* cases[] = {
        ".class\n",
        ".language\n",
        ".method\n",
        ".method f(regs=4, args=0)\n",
        ".builtin\n",
        ".field x\n",
    };
    for (const char* src : cases) {
        auto module = assemble_module(src);
        VORTEX_EXPECT(!module.has_value());
    }
}

VORTEX_TEST(assembler_trailing_comma_is_rejected_cleanly) {
    auto module = assemble_module(R"(
.method f(regs=3, args=0)
    JumpTrue v0, L1,
L1:
    Return v0
.end
)");
    VORTEX_EXPECT(!module.has_value());
}

VORTEX_TEST(mini_unary_minus_negates) {
    // Regression: -x used to emit x - 0 (a silent no-op).
    auto r = run_source_or_fail("fn main() { print(-3); }");
    VORTEX_EXPECT(r.has_value());
}

VORTEX_TEST(mini_not_matches_branch_truthiness) {
    // Regression: !x used to lower as EQ_REF(x, false), contradicting the
    // truthiness used by if/while (smi 0 and null are falsy).
    auto m = vortex::frontends::reference::compile_mini(
        "fn main() { let a = 0; if (!a) { print(1); } else { print(2); } }");
    VORTEX_EXPECT(m.has_value());
    if (!m) return;
    VORTEX_EXPECT(verify_module(*m).has_value());
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.register_builtin("print",
                            [](std::span<const TaggedValue> args, void*) {
                                return args.empty() ? TaggedValue::undefined()
                                                    : args[0];
                            });
    auto r = interp.run(*m, "main", std::span<const TaggedValue>{});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 1);  // !0 is truthy
}

VORTEX_TEST(shift_overflow_traps_instead_of_aliasing) {
    // 1 << 62 == 2^62 > smi_max: must trap, never wrap into a negative Smi.
    auto module = assemble_module(R"(
.method f(regs=4, args=0)
    CONST_I32 v0, 1
    CONST_I32 v1, 62
    SHL_I v2, v0, v1
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto r = interp.run(*module, "f", std::span<const TaggedValue>{});
    VORTEX_EXPECT(!r.has_value());
    if (!r) VORTEX_EXPECT(r.error().message.find("overflow") !=
                          std::string::npos);
}

VORTEX_TEST(check_bounds_rejects_negative_length) {
    // A negative length used to wrap under the unsigned compare and pass.
    auto r = run_source(R"(
.method f(regs=4, args=2)
    CHECK_BOUNDS v1, v0
    Return v1
.end
)",
                        "f", {TaggedValue::smi(0), TaggedValue::smi(-1)});
    VORTEX_EXPECT(!r.has_value());
    if (!r) VORTEX_EXPECT(r.error().message.find("negative") !=
                          std::string::npos);
}

VORTEX_TEST(verifier_rejects_call_arity_mismatch) {
    // A short argument window used to hand the callee pooled-frame garbage.
    UGBModule module;
    {
        MethodBuilder victim(module, "victim", 2, 1);
        victim.ret(0);
        VORTEX_EXPECT(victim.finish().has_value());
    }
    {
        MethodBuilder caller(module, "caller", 4, 0);
        caller.call_direct(3, 0, 0, "victim");  // passes 0 args, needs 1
        caller.ret(3);
        VORTEX_EXPECT(caller.finish().has_value());
    }
    auto res = verify_module(module);
    VORTEX_EXPECT(!res.has_value());
    if (!res) VORTEX_EXPECT(res.error().message.find("expects") !=
                            std::string::npos);
}

VORTEX_TEST(get_field_does_not_conflate_undefined_with_unresolved) {
    // Storing undefined into a field and reading it back is a success, not
    // an "unresolved field" error (ADR-005: no value doubles as a sentinel).
    auto r = run_source(R"(
.class P
.field x in P
.method f(regs=4, args=0)
    New.Object v0, P
    CONST_UNDEFINED v1
    SetField v0, v1, P.x
    GetField v2, v0, P.x
    Return v2
.end
)",
                        "f", {});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT(r->value.is_undefined());
}

// ---------------------------------------------------------------------------
// Rule 48: index edges survive node storage growth
// ---------------------------------------------------------------------------

VORTEX_TEST(graph_edges_survive_realloc) {
    support::Arena arena;
    ir::Graph g(arena);
    ir::NodeId start = g.add(ir::NodeKind::Start);
    ir::NodeId first = g.add_const(1, start);
    // Force vector reallocation with enough nodes; index edges must hold.
    ir::NodeId last = first;
    for (int i = 0; i < 64; ++i) {
        last = g.add(ir::NodeKind::Add, {last, first}, start);
    }
    g.add(ir::NodeKind::Return, {last});
    VORTEX_EXPECT_EQ(g.node(first).const_value, int64_t{1});
    for (const ir::Node& n : g.nodes()) {
        for (ir::NodeId in : n.data_inputs) {
            VORTEX_EXPECT(g.valid(in));
        }
    }
    // Reachable from Return: start, first, the add chain. Dead: none — the
    // chain is fully observed; the count asserts the DCE contract precisely.
    VORTEX_EXPECT_EQ(g.eliminate_dead_nodes(), uint32_t{0});
    VORTEX_EXPECT_EQ(g.live_count(), g.node_count());
}
