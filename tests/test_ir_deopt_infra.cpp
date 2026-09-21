// Arena, SoN graph DCE, RBPD region table, dependency graph, FFI handles,
// tiering policy, Mini reference frontend.
#include "vortex_test.hpp"

#include "vortex/deopt/rbpd.hpp"
#include "vortex/frontends/reference/emitter.hpp"
#include "vortex/gc/icggc.hpp"
#include "vortex/infra/dependency.hpp"
#include "vortex/infra/ffi.hpp"
#include "vortex/ir/son_graph.hpp"
#include "vortex/runtime/tiering.hpp"
#include "vortex/support/arena.hpp"
#include "vortex/ugb/verifier.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

VORTEX_TEST(arena_allocates_and_resets) {
    support::Arena arena(1024);
    int* a = arena.make<int>(7);
    VORTEX_EXPECT_EQ(*a, 7);
    int* arr = arena.make_array<int>(16);
    for (int i = 0; i < 16; ++i) arr[i] = i;
    VORTEX_EXPECT_EQ(arr[15], 15);
    VORTEX_EXPECT(arena.total_bytes() > 0);
    arena.reset();
    int* b = arena.make<int>(9);
    VORTEX_EXPECT_EQ(*b, 9);
}

// ---------------------------------------------------------------------------
// SoN graph
// ---------------------------------------------------------------------------

VORTEX_TEST(son_graph_dead_node_elimination) {
    support::Arena arena;
    ir::Graph g(arena);
    ir::Node* start = g.add(ir::NodeKind::Start);
    ir::Node* c1 = g.add(ir::NodeKind::Const, {}, start);
    ir::Node* c2 = g.add(ir::NodeKind::Const, {}, start);
    ir::Node* dead_add = g.add(ir::NodeKind::Add, {c1, c2});  // unused
    ir::Node* ret = g.add(ir::NodeKind::Return, {c1});
    (void)dead_add;
    (void)ret;

    const uint32_t killed = g.eliminate_dead_nodes();
    // dead_add AND the now-unreferenced c2 are unreachable from Return.
    VORTEX_EXPECT_EQ(killed, uint32_t{2});
    VORTEX_EXPECT(dead_add->dead);
    VORTEX_EXPECT(c2->dead);
    VORTEX_EXPECT(!c1->dead);
    VORTEX_EXPECT_EQ(g.live_count(), size_t{3});
}

// ---------------------------------------------------------------------------
// RBPD
// ---------------------------------------------------------------------------

VORTEX_TEST(rbpd_region_table_and_failure_protocol) {
    deopt::RegionTable table;
    deopt::RegionDescriptor d;
    d.kind = deopt::RegionKind::Hot;
    d.entry_offset = 0;
    d.exit_offset = 64;
    d.deopt.bytecode_resume_pc = 12;
    d.deopt.escape_locations.push_back(
        deopt::EscapeLocation{3, deopt::EscapeLocation::Loc::Reg, 0});
    const uint32_t id = table.add_region(d);
    VORTEX_EXPECT_EQ(id, uint32_t{0});
    VORTEX_EXPECT(table.find_by_pc_offset(32) != nullptr);
    VORTEX_EXPECT_EQ(table.find_by_pc_offset(64), nullptr);  // exclusive end

    // Guard failures: below threshold -> region recompile; at threshold ->
    // tier fallback (docs/deopt-rbpd.md section 4).
    VORTEX_EXPECT_EQ(table.on_guard_failure(id, 3), 0);
    VORTEX_EXPECT_EQ(table.on_guard_failure(id, 3), 0);
    VORTEX_EXPECT_EQ(table.on_guard_failure(id, 3), 2);
}

// ---------------------------------------------------------------------------
// Dependency graph (infra 1)
// ---------------------------------------------------------------------------

VORTEX_TEST(dependency_graph_invalidation_and_lazy) {
    infra::DependencyGraph graph;
    const infra::Assumption leaf{infra::AssumptionKind::KlassLeaf, 7};
    graph.register_assumption(leaf, {1, 0, true});
    graph.register_assumption(leaf, {2, 1, true});
    const infra::Assumption cold_leaf{infra::AssumptionKind::KlassLeaf, 7};
    graph.register_assumption(cold_leaf, {3, 2, false});
    VORTEX_EXPECT_EQ(graph.edge_count(), size_t{3});

    const infra::InvalidationBatch batch = graph.invalidate_klass(7);
    // Hot regions patch immediately; the cold one goes lazy and its edge is
    // retained until activation.
    VORTEX_EXPECT_EQ(batch.immediate.size(), size_t{2});
    VORTEX_EXPECT_EQ(batch.lazy.size(), size_t{1});
    VORTEX_EXPECT_EQ(graph.edge_count(), size_t{1});

    // Activation of the cold region fires the deferred invalidation.
    const infra::InvalidationBatch activated = graph.activate_region(3, 2);
    VORTEX_EXPECT_EQ(activated.immediate.size(), size_t{1});
}

VORTEX_TEST(dependency_graph_sweep_on_unload) {
    infra::DependencyGraph graph;
    graph.register_assumption(
        infra::Assumption{infra::AssumptionKind::MethodFinal, 100}, {1, 0, true});
    graph.register_assumption(
        infra::Assumption{infra::AssumptionKind::KlassLeaf, 200}, {1, 1, true});
    graph.register_assumption(
        infra::Assumption{infra::AssumptionKind::KlassLeaf, 200}, {2, 0, true});
    const size_t reclaimed = graph.sweep_methods({1});
    VORTEX_EXPECT_EQ(reclaimed, size_t{2});
    VORTEX_EXPECT_EQ(graph.edge_count(), size_t{1});
}

// ---------------------------------------------------------------------------
// FFI handles (infra 5)
// ---------------------------------------------------------------------------

VORTEX_TEST(handle_table_scopes) {
    infra::HandleTable table;
    int object = 1;
    auto h1 = table.create(&object, infra::HandleScope::Local);
    auto h2 = table.create(&object, infra::HandleScope::Global);
    VORTEX_EXPECT(h1.has_value() && h2.has_value());
    auto resolved = table.resolve(*h1);
    VORTEX_EXPECT(resolved.has_value() && resolved.value() == &object);
    VORTEX_EXPECT_EQ(table.live_count(), size_t{2});

    // Scope exit frees locals, keeps globals.
    VORTEX_EXPECT_EQ(table.destroy_scope(infra::HandleScope::Local), size_t{1});
    VORTEX_EXPECT_EQ(table.live_count(), size_t{1});
    auto dead = table.resolve(*h1);
    VORTEX_EXPECT(!dead.has_value());
    auto alive = table.resolve(*h2);
    VORTEX_EXPECT(alive.has_value());

    infra::PinningRegion pins;
    pins.pin(&object);
    VORTEX_EXPECT(pins.is_pinned(&object));
    pins.unpin(&object);
    VORTEX_EXPECT(!pins.is_pinned(&object));
}

// ---------------------------------------------------------------------------
// Tiering policy
// ---------------------------------------------------------------------------

VORTEX_TEST(tiering_policy_graduated_promotion) {
    TieringThresholds th;
    th.j1_invocations = 10;
    th.j2_invocations = 100;
    th.j3_invocations = 1000;
    th.j4_invocations = 10000;
    const TieringPolicy policy(th);

    MethodHotness h;
    h.current = Tier::T0;
    h.invocations = 5;
    VORTEX_EXPECT_EQ(policy.evaluate(h), Tier::T0);
    h.invocations = 10;
    VORTEX_EXPECT_EQ(policy.evaluate(h), Tier::J1);
    h.current = Tier::J1;
    h.invocations = 100;
    VORTEX_EXPECT_EQ(policy.evaluate(h), Tier::J2);

    // J4 requires a low deopt rate (docs/tier-j4.md section 10).
    h.current = Tier::J3;
    h.invocations = 10000;
    h.deopts = 100;  // 1% > 0.1% gate
    VORTEX_EXPECT_EQ(policy.evaluate(h), Tier::J3);
    h.deopts = 0;
    VORTEX_EXPECT_EQ(policy.evaluate(h), Tier::J4);
}

// ---------------------------------------------------------------------------
// ICGGC substrate
// ---------------------------------------------------------------------------

VORTEX_TEST(gc_tlab_allocation_and_cards) {
    gc::Heap heap(1 << 20);
    auto obj = heap.allocate_object(nullptr, 3);
    VORTEX_EXPECT(obj.has_value());
    if (obj) {
        // 16-byte alignment invariant of the tagged-value heap-pointer scheme.
        VORTEX_EXPECT_EQ(reinterpret_cast<uintptr_t>(obj.value()) % 16, size_t{0});
        (*obj)->field(0) = TaggedValue::smi(1);
        (*obj)->field(1) = TaggedValue::smi(2);
        (*obj)->field(2) = TaggedValue::smi(3);
        VORTEX_EXPECT_EQ((*obj)->field(1).as_smi(), 2);
    }
    auto arr = heap.allocate_array(8);
    VORTEX_EXPECT(arr.has_value());
    if (arr) VORTEX_EXPECT_EQ((*arr)->length(), uint32_t{8});
    VORTEX_EXPECT(heap.stats().objects_allocated >= 2);
}

// ---------------------------------------------------------------------------
// Reference frontend (Mini) end-to-end
// ---------------------------------------------------------------------------

VORTEX_TEST(mini_compile_verify_run_fib) {
    const char* src = R"(
fn fib(n) {
    let a = 0;
    let b = 1;
    while (n != 0) {
        let t = a + b;
        a = b;
        b = t;
        n = n - 1;
    }
    return a;
}

fn main() {
    let r = fib(25);
    print(r);
    return r;
}
)";
    auto module = frontends::reference::compile_mini(src);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    auto verified = ugb::verify_module(*module);
    VORTEX_EXPECT(verified.has_value());
    if (!verified) std::printf("  verify: %s\n", verified.error().message.c_str());

    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.register_builtin("print",
                            [](std::span<const TaggedValue>, void*) {
                                return TaggedValue::undefined();
                            });
    auto r = interp.run(*module, "main", {});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 75025);
}

VORTEX_TEST(mini_short_circuit_and_conditionals) {
    const char* src = R"(
fn pick(flag) {
    if (flag && 1) { return 10; } else { return 20; }
}
fn main() {
    let a = pick(true);
    let b = pick(false);
    return a + b;
}
)";
    auto module = frontends::reference::compile_mini(src);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto r = interp.run(*module, "main", {});
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 30);
}

VORTEX_TEST(mini_rejects_undefined_variable) {
    auto module = frontends::reference::compile_mini(
        "fn main() { return x; }");
    VORTEX_EXPECT(!module.has_value());
}
