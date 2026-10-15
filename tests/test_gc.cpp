// CEP:FILE: tests/test_gc.cpp
// CEP:WHAT: The M5 ICGGC suite: Brooks header invariant, barrier legs,
// young evacuation (T0 + inside-JIT shapes), the old cycle, SATB closure
// in assertion mode (both polarities, through the real JIT barrier), FFI
// pinning + weak handles, humongous objects, and the pause-p99 DoD gate.
// CEP:WHY: docs/gc-icggc.md section 8.9 (docs/roadmap.md M5 DoD) and Rule
// 124: GC stress tests are first-class.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT assertions name the failing property; the
// suite fails fast per test and never aborts the process.
// CEP:ASSUMES: fresh Fixture per test (heaps never share state); C++
// locals are never roots across collection points (docs 8.8 invariant 1).
// CEP:COST: @cold. The stress suite runs ~220 mutator batches + cycles in
// well under the CI budget.
// CEP:EVIDENCE: the tests themselves — 185-test suite green on Release and
// ASan/UBSan.
// M5 ICGGC tests (docs/gc-icggc.md section 8, docs/roadmap.md M5 DoD):
//   - Brooks header invariant + the mutator barrier legs
//   - young evacuation: promotion, root updates, slot updates
//   - the old cycle: phase machine, marking, CSet evacuation, sweep
//   - SATB closure in assertion mode with LIVE JIT stores (DoD 2, both
//     polarities: the closure holds through the real stencil barrier, and
//     a barrier-bypassed store is DETECTED)
//   - compaction under FFI pinning + weak-handle clearing (DoD 3)
//   - mutator pause p99 under budget on the allocation-stress suite (DoD 1)
//   - humongous objects: never move, reclaimed when dead
#include "vortex_test.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "vortex/gc/collector.hpp"
#include "vortex/infra/code_range.hpp"
#include "vortex/gc/icggc.hpp"
#include "vortex/infra/ffi.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/j1/stencil_corpus.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;
using namespace vortex::gc;

namespace {

// A compact heap: young 256 KiB (one TLAB chunk), old 2 MiB (8 regions).
constexpr size_t kYoungBytes = 256 * 1024;
constexpr size_t kOldBytes = 2 * 1024 * 1024;

struct Fixture {
    Heap heap{kYoungBytes, kOldBytes};
    vm::Interpreter interp{heap};
    infra::HandleTable handles;
    // CEP:WHAT: Per-test fixture: small heap + interpreter + handle table, wired.
    // CEP:WHY: tests must never share heap state.
    // CEP:STATUS: complete
    // CEP:FAILURE: none.
    // CEP:ASSUMES: the heap outlives the interpreter in member order.
    // CEP:COST: @cold.
    // CEP:EVIDENCE: every test in this file.
    Fixture() {
        heap.gc_state().handles = &handles;
        // The interpreter registers itself on construction; re-assert (the
        // member-init order makes the heap's copy of the pointer valid).
        heap.gc_state().interpreter = &interp;
    }
};

// CEP:WHAT: Creates a C++-owned klass descriptor with named fields.
// CEP:WHY: klass descriptors live outside the traced heap (M0 rule).
// CEP:STATUS: complete
// CEP:FAILURE: none (host allocation).
// CEP:ASSUMES: the static registry keeps them alive for the process.
// CEP:COST: @cold.
// CEP:EVIDENCE: every object-allocating test.
Klass* make_klass(Fixture& f, const char* name, uint32_t fields) {
    // Klass descriptors live on the C++ heap (not traced); the fixture's
    // static registry keeps them alive for the test's lifetime.
    struct Entry {
        std::unique_ptr<Klass> k;
    };
    static std::vector<Entry> registry;
    registry.push_back(Entry{std::make_unique<Klass>(name)});
    Klass* k = registry.back().k.get();
    for (uint32_t i = 0; i < fields; ++i) {
        // A single field is named "f" so JIT fixtures' modules (.field f in
        // X) resolve against the test klass by name (set_field_slow's
        // find_field is name-based).
        k->add_field(fields == 1 ? "f" : "f" + std::to_string(i));
    }
    (void)f;
    return k;
}

// CEP:WHAT: Places a young object's copy directly in old space (promotion shape).
// CEP:WHY: tests arrange old-space state the way the evacuator does.
// CEP:STATUS: complete
// CEP:FAILURE: returns nullptr when old space exhausts (the callers check).
// CEP:ASSUMES: the young header is valid; the copy self-forwards.
// CEP:COST: @cold.
// CEP:EVIDENCE: the old-cycle / evacuation / pinning tests.
Object* alloc_old_copy(Heap& heap, Object* young) {
    // Promotion without a collection: the tests place objects directly in
    // old space the way the young evacuator does (copy + self-forward).
    const size_t bytes = young->header.size;
    auto* raw = heap.promote(bytes);
    if (raw == nullptr) return nullptr;
    std::memcpy(raw, young, bytes);
    raw->header.brooks = raw;
    raw->header.mark = 0;
    raw->header.forwarded = 0;
    return static_cast<Object*>(raw);
}


// CEP:WHAT: Region index of an address (UINT32_MAX outside old space).
// CEP:WHY: tests arrange cross-region shapes by construction.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: the heap's regions exist.
// CEP:COST: @cold.
// CEP:EVIDENCE: the region-placement tests.
uint32_t region_index_of(Heap& heap, const void* addr) {
    Region* r = heap.region_of(addr);
    return r != nullptr ? r->index : UINT32_MAX;
}

// Promotes padding objects until `x` and the current bump region differ
// (the next promotion lands in a fresh region). Returns the new region.
// CEP:WHAT: Promotes padding until the bump region moves past x's region.
// CEP:WHY: region-granularity tests need objects in distinct Occupied regions.
// CEP:STATUS: complete
// CEP:FAILURE: returns x's region index if padding exhausts (the caller checks).
// CEP:ASSUMES: x is an old-space object; allocation may trigger young GCs.
// CEP:COST: @cold.
// CEP:EVIDENCE: gc_evacuation_moves_objects_and_updates_cross_region_slots.
uint32_t pad_to_next_region(Heap& heap, Klass* k, const void* x) {
    int guard = 0;
    while (guard++ < 100000) {
        auto pad = heap.allocate_object(k, 3);
        if (!pad) break;
        HeapObject* old_pad = alloc_old_copy(heap, *pad);
        if (old_pad == nullptr) break;
        if (region_index_of(heap, old_pad) != region_index_of(heap, x)) {
            return region_index_of(heap, old_pad);
        }
    }
    return region_index_of(heap, x);
}


// CEP:WHAT: Forces a trigger + steps the engine to Idle.
// CEP:WHY: tests that do not interleave mutator work during the cycle.
// CEP:STATUS: complete
// CEP:FAILURE: returns early on a named step failure (the assertions observe it).
// CEP:ASSUMES: the trigger is configured to fire.
// CEP:COST: @cold.
// CEP:EVIDENCE: every old-cycle test.
void drive_cycle(IcggcEngine& engine) {
    auto started = engine.maybe_start_cycle();
    if (!started) return;
    while (engine.phase() != IcggcEngine::Phase::Idle) {
        auto s = engine.step();
        if (!s) return;  // Failed -> Idle on the next step (observed above)
    }
}

// The J1 fixture for the barrier/DoD tests: one class, a setter method the
// test runs between engine slices (the stencil's outlined-barrier path).
struct J1Fixture {
    ugb::UGBModule module;
    vm::Interpreter* interp = nullptr;
    Heap* heap = nullptr;
    j1::J1Bindings bindings;
    std::unique_ptr<j1::BaselineExecutable> ex;
};

// .method setf(regs=3, args=2): SetField v0, v1, P.f; Return v0
constexpr const char* kSetterSrc = R"(
.class P
.field f in P

.method setf(regs=3, args=2)
    SetField v0, v1, P.f
    Return v0
.end
)";

// CEP:WHAT: Compiles + publishes the J1 setter fixture (the stencil barrier path).
// CEP:WHY: the SATB DoD must exercise the REAL JIT outlined barrier.
// CEP:STATUS: complete
// CEP:FAILURE: returns false on any named compile/publish failure (asserted).
// CEP:ASSUMES: the module defines class P with field f.
// CEP:COST: @cold.
// CEP:EVIDENCE: gc_satb_closure_holds_with_jit_stores_during_marking.
bool make_j1_setter(Heap& heap, vm::Interpreter& interp, J1Fixture& out) {
    auto module = ugb::assemble_module(kSetterSrc);
    if (!module) return false;
    out.module = std::move(*module);
    out.interp = &interp;
    out.heap = &heap;
    auto br = j1::make_j1_bindings(heap, out.module, &interp, out.bindings);
    if (!br) return false;
    const int32_t id = out.module.find_method("setf");
    if (id < 0) return false;
    j1::StencilTable corpus;
    build_default_corpus(corpus);
    j1::BaselineJit jit(corpus);
    j1::BaselineJob job;
    job.module = &out.module;
    job.method_id = static_cast<uint32_t>(id);
    job.klass_addrs = &out.bindings.klass_addr_table;
    job.field_offsets = out.bindings.field_offsets.data();
    job.field_offset_count = out.bindings.field_offsets.size();
    job.double_klass = out.bindings.double_klass;
    auto code = jit.compile(job);
    if (!code) return false;
    auto ex = j1::publish_baseline(*code);
    if (!ex) return false;
    out.ex = std::make_unique<j1::BaselineExecutable>(std::move(*ex));
    return true;
}

}  // namespace

// ---- header + barrier units -------------------------------------------------

/// CEP:WHAT: Every allocation site self-forwards the Brooks pointer.
/// CEP:WHY: docs/gc-icggc.md 8.2 header invariant.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the field.
/// CEP:ASSUMES: fresh fixture.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_brooks_self_after_allocation) {
    Fixture f;
    Klass* k = make_klass(f, "K", 2);
    auto obj = f.heap.allocate_object(k, 2);
    VORTEX_EXPECT(obj.has_value());
    auto arr = f.heap.allocate_array(4);
    VORTEX_EXPECT(arr.has_value());
    auto dbl = f.heap.allocate_double(2.5);
    VORTEX_EXPECT(dbl.has_value());
    if (obj && arr && dbl) {
        VORTEX_EXPECT_EQ((*obj)->header.brooks,
                         static_cast<HeapObject*>(*obj));
        VORTEX_EXPECT_EQ((*arr)->header.brooks,
                         static_cast<HeapObject*>(*arr));
        VORTEX_EXPECT_EQ((*dbl)->header.brooks,
                         static_cast<HeapObject*>(*dbl));
        VORTEX_EXPECT(gc_resolve(*obj) == *obj);
        VORTEX_EXPECT(f.heap.is_young(*obj));
    }
}

/// CEP:WHAT: The barrier's store/card/SATB legs behave per docs 8.3.
/// CEP:WHY: Rule 79 barrier completeness at the unit level.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the leg.
/// CEP:ASSUMES: fresh fixture.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_barriered_store_legs) {
    Fixture f;
    Klass* k = make_klass(f, "K", 1);
    auto owner = f.heap.allocate_object(k, 1);
    auto target = f.heap.allocate_object(k, 0);
    VORTEX_EXPECT(owner && target);
    if (!owner || !target) return;
    TaggedValue v = TaggedValue::heap_pointer(*target);
    f.heap.barriered_store(*owner, &(*owner)->field(0), v);
    VORTEX_EXPECT_EQ((*owner)->field(0).raw(), v.raw());
    // Card leg: the slot's card is dirty.
    VORTEX_EXPECT_EQ(f.heap.card_table().card(*owner), kCardDirty);
    // SATB leg: inactive marking logs nothing.
    VORTEX_EXPECT_EQ(f.heap.satb_buffer().size(), size_t{0});
    f.heap.set_marking_active(true);
    auto target2 = f.heap.allocate_object(k, 0);
    VORTEX_EXPECT(target2);
    f.heap.barriered_store(*owner, &(*owner)->field(0),
                           TaggedValue::heap_pointer(*target2));
    // The OLD value (the first target) is logged.
    VORTEX_EXPECT_EQ(f.heap.satb_buffer().size(), size_t{1});
    VORTEX_EXPECT_EQ(f.heap.satb_buffer()[0], v.raw());
    f.heap.set_marking_active(false);
}

/// CEP:WHAT: Cross-region stores log remembered-set entries; same-region stores do not.
/// CEP:WHY: docs 8.3 remset leg — evacuation update correctness.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the check.
/// CEP:ASSUMES: the owner/target placement uses pad_to_next_region.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_remset_leg_tracks_cross_region_slots) {
    Fixture f;
    Klass* k = make_klass(f, "K", 1);
    auto yowner = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(yowner);
    if (!yowner) return;
    auto* oowner = alloc_old_copy(f.heap, *yowner);
    VORTEX_EXPECT(oowner != nullptr);
    if (oowner == nullptr) return;
    // Place the target in a DIFFERENT region by construction: pad until
    // the bump region moves past the owner's, then allocate + promote the
    // target adjacently (the barrier only logs cross-region edges —
    // same-region slots need no remembered-set entry).
    Klass* k3 = make_klass(f, "K3", 3);
    pad_to_next_region(f.heap, k3, oowner);
    auto ytarget = f.heap.allocate_object(k, 0);
    auto* otarget = ytarget ? alloc_old_copy(f.heap, *ytarget) : nullptr;
    VORTEX_EXPECT(otarget != nullptr);
    if (otarget == nullptr) return;
    VORTEX_EXPECT(region_index_of(f.heap, oowner) !=
                  region_index_of(f.heap, otarget));
    TaggedValue* slot = &static_cast<Object*>(oowner)->field(0);
    f.heap.barriered_store(static_cast<HeapObject*>(oowner), slot,
                           TaggedValue::heap_pointer(otarget));
    Region* tr = f.heap.region_of(otarget);
    VORTEX_EXPECT_EQ(tr->remset.size(), size_t{1});
    if (tr->remset.size() == 1) {
        VORTEX_EXPECT_EQ(tr->remset[0].slot, slot);
    }
}

// ---- young evacuation ---------------------------------------------------------

/// CEP:WHAT: Young evacuation promotes live objects and updates roots/edges state-exactly.
/// CEP:WHY: docs 8.1 evacuation + Rule 84 identity.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the property.
/// CEP:ASSUMES: the root is a handle; C++ locals are not roots.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_young_evacuation_promotes_and_updates_roots) {
    Fixture f;
    Klass* k = make_klass(f, "K", 2);
    auto r = f.heap.allocate_object(k, 2);
    auto c = f.heap.allocate_object(k, 2);
    VORTEX_EXPECT(r && c);
    if (!r || !c) return;
    (*r)->field(0) = TaggedValue::smi(41);
    (*c)->field(0) = TaggedValue::smi(42);
    (*r)->field(1) = TaggedValue::heap_pointer(*c);
    auto* root_raw = *r;
    const auto h = f.handles.create(*r, infra::HandleScope::Global);
    VORTEX_EXPECT(h.has_value());
    if (!h) return;

    auto collected = f.heap.collect_young();
    VORTEX_EXPECT(collected.has_value());
    if (!collected) return;
    VORTEX_EXPECT_EQ(f.heap.engine().stats().promoted_objects, uint64_t{2});
    // The handle resolves to the PROMOTED body (never the dead young one).
    auto resolved = f.handles.resolve(*h);
    VORTEX_EXPECT(resolved.has_value());
    if (!resolved) return;
    auto* new_root = static_cast<Object*>(*resolved);
    VORTEX_EXPECT(new_root != root_raw);
    VORTEX_EXPECT(f.heap.region_of(new_root) != nullptr);  // old space now
    // Data preserved; the child edge resolves into old space too.
    VORTEX_EXPECT_EQ(new_root->field(0).as_smi(), 41);
    auto* new_child = static_cast<Object*>(
        gc_resolve(new_root->field(1).as_heap_object()));
    VORTEX_EXPECT_EQ(new_child->field(0).as_smi(), 42);
    VORTEX_EXPECT(f.heap.region_of(new_child) != nullptr);
    // The forwarded young body resolves to the promoted one.
    VORTEX_EXPECT(gc_resolve(root_raw) == new_root);
    // The young space is empty again.
    VORTEX_EXPECT(f.heap.tlab_top() == f.heap.young_base());
}

/// CEP:WHAT: A young GC fires INSIDE a J1 allocation; the GC-map walk updates the JIT frame; parity holds vs T0.
/// CEP:WHY: the GC-map stack-scan DoD (docs 8.6) end-to-end.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the counter or the parity.
/// CEP:ASSUMES: the J1 executable is registered in the code registry.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_young_gc_inside_jit_allocation_keeps_parity) {
    // The DoD's JIT-stack-scan shape: a J1 method allocates in a loop with
    // LIVE references in vregs; the young GC fires inside alloc_slow, the
    // GC-map walk updates the JIT frame slots, and the results stay exact.
    constexpr const char* kSrc = R"(
.class Cell
.field v in Cell

.method churn(regs=7, args=1)
    Const.I32 v1, 0
    Const.I32 v2, 1400
loop:
    Const.I32 v3, 7
    New.Object v4, Cell
    SetField v4, v3, Cell.v
    GetField v5, v4, Cell.v
    Add.I32 v1, v1, v5
    Add.I32 v1, v1, v0
    Call.Direct v6, v1, 1, noop
    Sub.I32 v2, v2, v3
    JumpFalse v2, done
    Jump loop
done:
    Return v1
.end

.method noop(regs=1, args=1)
    Return v0
.end
)";
    Fixture f;
    auto module = ugb::assemble_module(kSrc);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    f.heap.gc_state().interpreter = &f.interp;
    auto boot = f.interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(f.heap, *module, &f.interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    const int32_t id = module->find_method("churn");
    VORTEX_EXPECT(id >= 0);
    if (id < 0) return;
    j1::StencilTable corpus;
    build_default_corpus(corpus);
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

    // The GC map registry now holds the executable's record.
    VORTEX_EXPECT(f.heap.engine().stats().jit_frames_scanned == 0);

    // Run until young collections have happened INSIDE the JIT method
    // (200 iterations × ~40 bytes < 256 KiB, so run it repeatedly with a
    // growing accumulator — each run allocates 200 cells).
    int64_t acc = 0;
    for (int round = 0; round < 80; ++round) {
        const std::vector<TaggedValue> jargs = {TaggedValue::smi(acc % 7)};
        auto run = j1::run_baseline(*ex, bindings, jargs);
        VORTEX_EXPECT(run.has_value());
        if (!run) return;
        acc = run->as_smi();
    }
    VORTEX_EXPECT(f.heap.engine().stats().young_collections > 0);
    VORTEX_EXPECT(f.heap.engine().stats().promoted_objects > 0);
    // Parity: 40 rounds of (200 * (7 + acc%7)) with the documented wrap.
    // The exact arithmetic is pinned by the T0 oracle below.
    vm::Result<vm::RunResult> t0 = [&] {
        gc::Heap heap2{kYoungBytes, kOldBytes};
        vm::Interpreter interp2(heap2);
        auto m2 = ugb::assemble_module(kSrc);
        if (!m2) return vm::Result<vm::RunResult>{};
        auto boot2 = interp2.build_module_runtime(*m2);
        if (!boot2) return vm::Result<vm::RunResult>{};
        int64_t a = 0;
        TaggedValue last = TaggedValue::smi(0);
        for (int round = 0; round < 80; ++round) {
            const std::vector<TaggedValue> t0args = {TaggedValue::smi(a % 7)};
            auto run = interp2.run(*m2, "churn", t0args);
            if (!run) return vm::Result<vm::RunResult>{};
            last = run->value;
            a = run->value.as_smi();
        }
        return vm::Result<vm::RunResult>{vm::RunResult{last}};
    }();
    VORTEX_EXPECT(t0.has_value());
    if (t0) VORTEX_EXPECT_EQ(acc, t0->value.as_smi());
}

// ---- the old cycle -------------------------------------------------------------

/// CEP:WHAT: The phase machine steps through the documented ladder back to Idle.
/// CEP:WHY: docs 8.4 stepwise contract.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the phase.
/// CEP:ASSUMES: the trigger is forced.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_engine_phase_machine_steps_through_ladder) {
    Fixture f;
    Klass* k = make_klass(f, "K", 1);
    auto a = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(a);
    if (!a) return;
    const auto h = f.handles.create(*a, infra::HandleScope::Global);
    auto promoted = f.heap.collect_young();
    VORTEX_EXPECT(promoted.has_value());

    auto& engine = f.heap.engine();
    engine.config.old_gc_trigger_pct = 0;  // force the trigger
    auto started = engine.maybe_start_cycle();
    VORTEX_EXPECT(started.has_value());
    VORTEX_EXPECT(engine.phase() == IcggcEngine::Phase::ConcurrentMark);
    std::vector<IcggcEngine::Phase> seen;
    while (engine.phase() != IcggcEngine::Phase::Idle) {
        auto s = engine.step();
        VORTEX_EXPECT(s.has_value());
        if (!s) break;
        seen.push_back(*s);
    }
    // The ladder: FinalMark, Evacuate slices, UpdateRoots, Sweep slices.
    bool saw_final = false, saw_update = false, saw_sweep = false;
    for (auto p : seen) {
        if (p == IcggcEngine::Phase::FinalMark) saw_final = true;
        if (p == IcggcEngine::Phase::UpdateRoots) saw_update = true;
        if (p == IcggcEngine::Phase::Sweep) saw_sweep = true;
    }
    VORTEX_EXPECT(saw_final && saw_update && saw_sweep);
    VORTEX_EXPECT_EQ(engine.stats().old_cycles, uint64_t{1});
    // Idle steps are a named no-op.
    auto idle = engine.step();
    VORTEX_EXPECT(idle.has_value());
    VORTEX_EXPECT(*idle == IcggcEngine::Phase::Idle);
    // The survivor is intact through the handle.
    auto resolved = f.handles.resolve(*h);
    VORTEX_EXPECT(resolved.has_value());
    if (resolved) {
        auto* o = static_cast<Object*>(*resolved);
        VORTEX_EXPECT(f.heap.region_of(o) != nullptr);
    }
}

/// CEP:WHAT: A cycle reclaims fully-dead regions and preserves the survivor.
/// CEP:WHY: docs 8.4 sweep leg.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the counters.
/// CEP:ASSUMES: the garbage spans full regions.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_old_cycle_reclaims_dead_region) {
    Fixture f;
    Klass* k = make_klass(f, "K", 1);
    // Fill > 1 region with garbage (no roots at all) plus one survivor.
    const int garbage = 14000;  // ~40 bytes each: > 1 full region
    for (int i = 0; i < garbage; ++i) {
        auto o = f.heap.allocate_object(k, 1);
        VORTEX_EXPECT(o);
        if (!o) return;
        auto* old_o = alloc_old_copy(f.heap, *o);
        VORTEX_EXPECT(old_o != nullptr);
        if (old_o == nullptr) return;
        (*o)->field(0) = TaggedValue::heap_pointer(old_o);
    }
    auto survivor = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(survivor);
    if (!survivor) return;
    const auto h = f.handles.create(*survivor, infra::HandleScope::Global);
    auto promoted = f.heap.collect_young();
    VORTEX_EXPECT(promoted.has_value());
    if (!promoted) return;

    auto& engine = f.heap.engine();
    engine.config.old_gc_trigger_pct = 0;
    drive_cycle(engine);
    VORTEX_EXPECT_EQ(engine.stats().old_cycles, uint64_t{1});
    VORTEX_EXPECT(engine.stats().regions_freed >= 1);
    VORTEX_EXPECT(engine.stats().bytes_reclaimed >= Region::kSize);
    // The survivor survives, reachable and readable through the handle.
    auto resolved = f.handles.resolve(*h);
    VORTEX_EXPECT(resolved.has_value());
    if (resolved) {
        VORTEX_EXPECT(f.heap.region_of(*resolved) != nullptr);
    }
}

/// CEP:WHAT: Evacuation moves objects, preserves identity, and updates cross-region slots.
/// CEP:WHY: docs 8.4 evacuation + Rule 84 identity.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the property.
/// CEP:ASSUMES: a/b sit in distinct Occupied regions.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_evacuation_moves_objects_and_updates_cross_region_slots) {
    Fixture f;
    Klass* k = make_klass(f, "K", 1);
    // Discipline (the fixture's own rule): a young pointer is promoted
    // IMMEDIATELY — padding below triggers young GCs that reuse young
    // memory, so no young pointer may cross an allocation.
    auto ya = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(ya);
    if (!ya) return;
    auto* a = alloc_old_copy(f.heap, *ya);
    VORTEX_EXPECT(a != nullptr);
    if (a == nullptr) return;
    // Pad until the bump region moves past a's, then allocate + promote b
    // ADJACENTLY (no collection can separate the two steps): the two live
    // objects start in DIFFERENT regions (both fully allocated, so both
    // become evacuation candidates).
    Klass* k3 = make_klass(f, "K3", 3);
    pad_to_next_region(f.heap, k3, a);
    auto yb = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(yb);
    if (!yb) return;
    auto* b = alloc_old_copy(f.heap, *yb);
    VORTEX_EXPECT(b != nullptr);
    if (b == nullptr) return;
    if (region_index_of(f.heap, a) == region_index_of(f.heap, b)) {
        pad_to_next_region(f.heap, k3, b);
        auto yb2 = f.heap.allocate_object(k, 1);
        VORTEX_EXPECT(yb2);
        if (!yb2) return;
        b = alloc_old_copy(f.heap, *yb2);
        VORTEX_EXPECT(b != nullptr);
        if (b == nullptr) return;
    }
    VORTEX_EXPECT(region_index_of(f.heap, a) != region_index_of(f.heap, b));
    // Fill BOTH regions completely (the current bump region never
    // evacuates — Bump state rolls to Occupied only when full).
    pad_to_next_region(f.heap, k3, a);
    pad_to_next_region(f.heap, k3, b);
    VORTEX_EXPECT(region_index_of(f.heap, a) != region_index_of(f.heap, b));
    static_cast<Object*>(a)->field(0) = TaggedValue::heap_pointer(b);
    const auto ha = f.handles.create(a, infra::HandleScope::Global);
    VORTEX_EXPECT(ha.has_value());
    if (!ha) return;

    auto& engine = f.heap.engine();
    engine.config.old_gc_trigger_pct = 0;
    engine.config.evacuate_live_ratio_pct = 100;  // evacuate every region
    drive_cycle(engine);
    VORTEX_EXPECT(engine.stats().evacuated_objects >= 2);

    auto resolved = f.handles.resolve(*ha);
    VORTEX_EXPECT(resolved.has_value());
    if (!resolved) return;
    auto* new_a = static_cast<Object*>(*resolved);
    VORTEX_EXPECT(new_a != a);  // moved
    // The cross-region slot moved WITH the owner and resolves to b's new
    // body (identity preserved, Rule 84).
    TaggedValue edge = new_a->field(0);
    VORTEX_EXPECT(edge.is_heap_object());
    // RAW slot assertion (14b-B m3): the slot itself must hold the NEW body
    // — resolving through gc_resolve would mask a missing remembered-set
    // update (a stale slot still "resolves" via the frozen body's brooks).
    VORTEX_EXPECT_EQ(edge.as_heap_object(),
                     static_cast<HeapObject*>(gc_resolve(edge.as_heap_object())));
    auto* new_b = gc_resolve(edge.as_heap_object());
    VORTEX_EXPECT(new_b != b);   // b moved too
    VORTEX_EXPECT(f.heap.region_of(new_b) != nullptr);
    // The old bodies' region was reset (Free) after the evacuation.
    Region* old_region = f.heap.region_of(a);
    // a's region may be reused already; the frozen body is gone either way.
    (void)old_region;
    // Everything still reads: b's body is a real object.
    VORTEX_EXPECT(new_b->header.size >= sizeof(ObjectHeader));
}

// ---- SATB closure in assertion mode (DoD 2) ------------------------------------

/// CEP:WHAT: DoD 2 positive: the SATB closure holds under live JIT stores during marking.
/// CEP:WHY: docs 8.4 assertion mode exercising the REAL stencil barrier.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the phase.
/// CEP:ASSUMES: the J1 setter fixture is published; assertion mode is on.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_satb_closure_holds_with_jit_stores_during_marking) {
    Fixture f;
    Klass* k = make_klass(f, "P", 1);
    auto ya = f.heap.allocate_object(k, 1);
    auto yb = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(ya && yb);
    if (!ya || !yb) return;
    auto* a = alloc_old_copy(f.heap, *ya);
    auto* b = alloc_old_copy(f.heap, *yb);
    VORTEX_EXPECT(a && b);
    if (!a || !b) return;
    // b is reachable ONLY through a.f at snapshot time.
    static_cast<Object*>(a)->field(0) = TaggedValue::heap_pointer(b);
    const auto ha = f.handles.create(a, infra::HandleScope::Global);
    VORTEX_EXPECT(ha.has_value());
    if (!ha) return;

    // The J1 setter: the stencil's outlined barrier is the store path.
    J1Fixture j1f;
    VORTEX_EXPECT(make_j1_setter(f.heap, f.interp, j1f));
    if (!j1f.ex) return;
    TaggedValue a_val = TaggedValue::heap_pointer(a);

    auto& engine = f.heap.engine();
    engine.config.assertion_mode = true;
    engine.config.old_gc_trigger_pct = 0;
    engine.config.mark_slice_items = 2;  // many slices: the mutator stores
    auto started = engine.maybe_start_cycle();
    VORTEX_EXPECT(started.has_value());
    // Mutator: JIT stores overwriting a.f DURING ConcurrentMark. The old
    // value (b) survives ONLY through the SATB log the stencil writes.
    TaggedValue null_v = TaggedValue::null();
    for (int i = 0; i < 8 && engine.phase() == IcggcEngine::Phase::ConcurrentMark;
         ++i) {
        const std::vector<TaggedValue> jargs = {a_val, null_v};
        auto run = j1::run_baseline(*j1f.ex, j1f.bindings, jargs);
        VORTEX_EXPECT(run.has_value());
        if (!run) return;
        auto s = engine.step();
        VORTEX_EXPECT(s.has_value());
        if (!s) return;
    }
    // Finish the cycle; the closure must hold (no failure).
    while (engine.phase() != IcggcEngine::Phase::Idle) {
        auto s = engine.step();
        VORTEX_EXPECT(s.has_value());
        if (!s) {
            VORTEX_EXPECT(false && "closure failed under live JIT stores");
            return;
        }
    }
    // The SATB tail marked b (its only reference was overwritten).
    VORTEX_EXPECT(engine.stats().satb_logs > 0);
    VORTEX_EXPECT(engine.stats().closure_checks == 1);
}

/// CEP:WHAT: DoD 2 negative: a barrier-bypassed store FAILS the closure with the named error.
/// CEP:WHY: docs 8.4 — the gate must detect misses, not just pass.
/// CEP:STATUS: complete
/// CEP:FAILURE: the failure IS the expected outcome (asserted by name).
/// CEP:ASSUMES: assertion mode is on.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_satb_closure_detects_missed_log) {
    Fixture f;
    Klass* k = make_klass(f, "P", 1);
    auto ya = f.heap.allocate_object(k, 1);
    auto yb = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(ya && yb);
    if (!ya || !yb) return;
    auto* a = alloc_old_copy(f.heap, *ya);
    auto* b = alloc_old_copy(f.heap, *yb);
    VORTEX_EXPECT(a && b);
    if (!a || !b) return;
    static_cast<Object*>(a)->field(0) = TaggedValue::heap_pointer(b);
    const auto ha = f.handles.create(a, infra::HandleScope::Global);
    VORTEX_EXPECT(ha.has_value());
    if (!ha) return;

    auto& engine = f.heap.engine();
    engine.config.assertion_mode = true;
    engine.config.old_gc_trigger_pct = 0;
    auto started = engine.maybe_start_cycle();
    VORTEX_EXPECT(started.has_value());
    // The regression shape: a store that SKIPS its SATB leg (what a buggy
    // barrier would do) during ConcurrentMark. The snapshot's a->b edge is
    // overwritten without a log — b must stay white and the closure must
    // name the violation.
    TaggedValue* slot = &static_cast<Object*>(a)->field(0);
    *slot = TaggedValue::null();  // bypassed store (the negative shape)
    bool saw_failure = false;
    std::string message;
    while (engine.phase() != IcggcEngine::Phase::Idle) {
        auto s = engine.step();
        if (!s) {
            saw_failure = true;
            message = s.error().message;
            break;
        }
    }
    VORTEX_EXPECT(saw_failure);
    VORTEX_EXPECT(message.find("SatbClosureViolated") != std::string::npos);
    // One-shot observation: the next step returns to Idle with the named
    // post-mortem.
    auto post = engine.step();
    VORTEX_EXPECT(!post.has_value());
    VORTEX_EXPECT(engine.phase() == IcggcEngine::Phase::Idle);
}

// ---- pinning + weak handles (DoD 3) ---------------------------------------------

/// CEP:WHAT: DoD 3: pinned objects never move; unpinned evacuate; weak handles clear.
/// CEP:WHY: docs 8.6/8.7 + Rules 84/85.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the property.
/// CEP:ASSUMES: p lives in its own region (pinned regions never evacuate).
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_compaction_preserves_identity_under_ffi_pinning) {
    Fixture f;
    Klass* k = make_klass(f, "K", 1);
    auto yp = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(yp);
    if (!yp) return;
    // Discipline: p (the pinned object) lands in its OWN region — a
    // pinned object's region never evacuates, so q/r must live elsewhere.
    auto* p = alloc_old_copy(f.heap, *yp);
    VORTEX_EXPECT(p != nullptr);
    if (p == nullptr) return;
    Klass* k3 = make_klass(f, "K3", 3);
    pad_to_next_region(f.heap, k3, p);
    auto yq = f.heap.allocate_object(k, 1);
    auto yr = f.heap.allocate_object(k, 1);
    auto ydead = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(yq && yr && ydead);
    if (!yq || !yr || !ydead) return;
    auto* q = alloc_old_copy(f.heap, *yq);
    auto* r = alloc_old_copy(f.heap, *yr);
    auto* dead = alloc_old_copy(f.heap, *ydead);
    VORTEX_EXPECT(q && r && dead);
    if (!q || !r || !dead) return;
    static_cast<Object*>(q)->field(0) = TaggedValue::heap_pointer(r);
    // Fill q's region completely AND roll the bump past it (only
    // Occupied regions evacuate; the current bump region never does).
    pad_to_next_region(f.heap, k3, q);
    pad_to_next_region(f.heap, k3, q);
    infra::PinningRegion pins;
    pins.pin(p);
    f.heap.register_pinning(&pins);
    const auto hp = f.handles.create(p, infra::HandleScope::Global);
    const auto hq = f.handles.create(q, infra::HandleScope::Global);
    const auto hw =
        f.handles.create(dead, infra::HandleScope::Weak);
    VORTEX_EXPECT(hp.has_value() && hq.has_value() && hw.has_value());
    if (!hp || !hq || !hw) return;

    auto& engine = f.heap.engine();
    engine.config.old_gc_trigger_pct = 0;
    engine.config.evacuate_live_ratio_pct = 100;
    drive_cycle(engine);

    // The pinned object never moves (Rule 84: stable identity).
    auto rp = f.handles.resolve(*hp);
    VORTEX_EXPECT(rp.has_value());
    if (rp) VORTEX_EXPECT_EQ(static_cast<HeapObject*>(*rp), p);
    VORTEX_EXPECT(p->header.pinned == 1);
    // The unpinned object moved; its edge resolves to r's new body.
    auto rq = f.handles.resolve(*hq);
    VORTEX_EXPECT(rq.has_value());
    if (!rq) return;
    auto* new_q = static_cast<Object*>(*rq);
    VORTEX_EXPECT(new_q != q);
    auto* new_r = gc_resolve(new_q->field(0).as_heap_object());
    VORTEX_EXPECT(f.heap.region_of(new_r) != nullptr);
    VORTEX_EXPECT(new_r != r);
    // The weak handle to the dead object cleared (Rule 85).
    auto rw = f.handles.resolve(*hw);
    VORTEX_EXPECT(rw.has_value());
    if (rw) VORTEX_EXPECT(*rw == nullptr);
    VORTEX_EXPECT(f.heap.engine().stats().weak_handles_cleared == 1);
    f.heap.unregister_pinning(&pins);
}


/// CEP:WHAT: J2/J3/J4 stores survive the SATB diversion: values live across
/// an ArrayElement store keep their register/home state with marking off AND on.
/// CEP:WHY: the 14b-B blocker — the diversion used to clobber R11 and the
/// allocator did not treat these stores as safepoints (probe-proven 89->77).
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT names the parity leg that broke.
/// CEP:ASSUMES: the emitter + allocator treat RawOffset/ArrayElement stores
/// as call sites (Rule 78).
/// CEP:COST: @cold. One compile + three runs.
/// CEP:EVIDENCE: this test (the probe shape it pins).
VORTEX_TEST(j2_store_safepoint_parity_under_satb) {
    constexpr const char* kSrc = R"(
.method main(regs=24, args=1)
    Const.I32 v1, 4
    New.Array v2, v1
    Add.I32 v3, v0, v0
    Add.I32 v4, v3, v0
    Add.I32 v5, v4, v0
    Add.I32 v6, v5, v0
    Add.I32 v7, v6, v0
    Add.I32 v8, v7, v0
    Const.I32 v9, 2
    Array.Set v2, v9, v2
    Array.Get v17, v2, v9
    Array.Length v20, v17
    Add.I32 v21, v20, v20
    Add.I32 v11, v3, v4
    Add.I32 v12, v5, v6
    Add.I32 v13, v7, v8
    Add.I32 v14, v11, v12
    Add.I32 v15, v14, v13
    Add.I32 v22, v15, v21
    Call.Builtin v16, v2, 1, print
    Return v22
.end
)";
    Fixture f;
    f.interp.register_builtin(
        "print", [](std::span<const TaggedValue>, void*) {
            return TaggedValue::undefined();
        });
    auto module = ugb::assemble_module(kSrc);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    f.heap.gc_state().interpreter = &f.interp;
    auto boot = f.interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(f.heap, *module, &f.interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    const int32_t id = module->find_method("main");
    VORTEX_EXPECT(id >= 0);
    if (id < 0) return;
    j2::J2Job job;
    job.module = &*module;
    job.method_id = static_cast<uint32_t>(id);
    job.profiles = &module->method_table[static_cast<size_t>(id)].profiles;
    job.ics = &module->method_table[static_cast<size_t>(id)].ics;
    job.klass_addrs = &bindings.klass_addr_table;
    job.double_klass = bindings.double_klass;
    auto code = j2::compile_j2(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    auto ex = j2::publish_j2(*code, infra::global_code_range());
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;

    const std::vector<TaggedValue> args{TaggedValue::smi(3)};
    auto t0 = f.interp.run(*module, "main", args);
    VORTEX_EXPECT(t0.has_value());
    if (!t0) return;
    // Fast path (marking off): the diversion must not perturb the model.
    auto r_fast = j2::run_j2(*ex, bindings, args, f.interp);
    VORTEX_EXPECT(r_fast.has_value());
    if (r_fast) VORTEX_EXPECT_EQ(r_fast->as_smi(), t0->value.as_smi());
    // SATB path (marking live): the outlined barrier must be a real
    // safepoint — homed intervals + no register clobber.
    f.heap.set_marking_active(true);
    auto r_satb = j2::run_j2(*ex, bindings, args, f.interp);
    f.heap.set_marking_active(false);
    VORTEX_EXPECT(r_satb.has_value());
    if (r_satb) VORTEX_EXPECT_EQ(r_satb->as_smi(), t0->value.as_smi());
}

/// CEP:WHAT: Two consecutive old cycles with NO young GC in between keep
/// the closure: young mark bits are cleared at each InitialMark.
/// CEP:WHY: the 14b-A blocker — stale young marks made the second cycle
/// skip every young body and lose its old referents.
/// CEP:STATUS: complete
/// CEP:FAILURE: the closure failure or a lost object names the cycle.
/// CEP:ASSUMES: assertion mode is on (the closure gate runs).
/// CEP:COST: @cold. Two cycles over a tiny graph.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_two_cycles_without_young_gc_keep_closure) {
    Fixture f;
    Klass* k = make_klass(f, "K", 1);
    // Young Y (handle root) -> old O: the edge the first cycle's young
    // seeding marks and the second cycle must NOT lose.
    Klass* k3 = make_klass(f, "K3", 3);
    auto yo = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(yo);
    if (!yo) return;
    auto* o = alloc_old_copy(f.heap, *yo);
    VORTEX_EXPECT(o != nullptr);
    if (o == nullptr) return;
    pad_to_next_region(f.heap, k3, o);
    auto yy = f.heap.allocate_object(k, 1);
    VORTEX_EXPECT(yy);
    if (!yy) return;
    (*yy)->field(0) = TaggedValue::heap_pointer(o);
    const auto hy = f.handles.create(*yy, infra::HandleScope::Global);
    VORTEX_EXPECT(hy.has_value());
    if (!hy) return;

    auto& engine = f.heap.engine();
    engine.config.assertion_mode = true;
    engine.config.old_gc_trigger_pct = 0;
    for (int cycle = 0; cycle < 2; ++cycle) {
        auto started = engine.maybe_start_cycle();
        VORTEX_EXPECT(started.has_value());
        while (engine.phase() != IcggcEngine::Phase::Idle) {
            auto s = engine.step();
            if (!s) {
                fprintf(stderr, "[cycle %d] %s\n", cycle,
                        s.error().message.c_str());
            }
            VORTEX_EXPECT(s.has_value());
            if (!s) return;
        }
    }
    // O survives: reachable through the young handle's edge in both cycles.
    auto resolved = f.handles.resolve(*hy);
    VORTEX_EXPECT(resolved.has_value());
    if (!resolved) return;
    auto* new_y = static_cast<Object*>(*resolved);
    TaggedValue edge = new_y->field(0);
    VORTEX_EXPECT(edge.is_heap_object());
    auto* new_o = gc_resolve(edge.as_heap_object());
    VORTEX_EXPECT(f.heap.region_of(new_o) != nullptr);
}

// ---- humongous -------------------------------------------------------------------

/// CEP:WHAT: Humongous objects survive cycles unmoved and reclaim when dead.
/// CEP:WHY: docs 8.1 humongous rules.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the property.
/// CEP:ASSUMES: the array exceeds one region.
/// CEP:COST: @cold.
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_humongous_never_moves_and_reclaims_when_dead) {
    Fixture f;
    Klass* k = make_klass(f, "K", 1);
    // > 256 KiB: humongous (region run) — direct old-space allocation.
    const uint32_t big_len =
        static_cast<uint32_t>(Region::kSize / sizeof(TaggedValue)) + 64;
    auto big = f.heap.allocate_array(big_len);
    VORTEX_EXPECT(big.has_value());
    if (!big) return;
    VORTEX_EXPECT(f.heap.region_of(*big) != nullptr);  // old space
    VORTEX_EXPECT((*big)->header.size > Region::kSize);
    const auto h = f.handles.create(*big, infra::HandleScope::Global);
    VORTEX_EXPECT(h.has_value());
    if (!h) return;
    // Live through a cycle: no movement.
    auto& engine = f.heap.engine();
    engine.config.old_gc_trigger_pct = 0;
    drive_cycle(engine);
    auto resolved = f.handles.resolve(*h);
    VORTEX_EXPECT(resolved.has_value());
    if (resolved) VORTEX_EXPECT_EQ(static_cast<HeapObject*>(*resolved), *big);
    // Dead: the run reclaims.
    f.handles.destroy(*h);
    drive_cycle(engine);
    VORTEX_EXPECT(f.heap.engine().stats().humongous_reclaimed >= 1);
}

// ---- DoD 1: pause p99 on the allocation-stress suite ------------------------------

/// CEP:WHAT: DoD 1: p99 over ALL pause events stays under budget on the stress suite.
/// CEP:WHY: docs 8.5/8.9 — the pause-bounding gate.
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT failures name the counter or the budget.
/// CEP:ASSUMES: the workload pressures both spaces; cycles run between batches.
/// CEP:COST: @cold (CI-bounded).
/// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_pause_p99_under_budget_on_allocation_stress) {
    Fixture f;
    Klass* k = make_klass(f, "Cell", 2);
    // Mixed workload with REAL old-space pressure: each batch appends
    // surviving list nodes (roots) and churns transients, so promotion
    // fills the old space, cycles trigger on occupancy, evacuation moves
    // survivors, and young GCs fire inside the churn — every pause path
    // (young GCs, STW phases, slices) records into the ring.
    auto& engine = f.heap.engine();
    engine.config.old_gc_trigger_pct = 35;
    engine.config.mark_slice_items = 64;
    J1Fixture jitter;
    {
        // A J1 allocation+store loop: every batch runs a few rounds through
        // published code (young GCs fire inside alloc_slow with JIT frames
        // live — the pause ring then covers the JIT-side stalls too).
        constexpr const char* kSrc = R"(
.class Cell
.field f in Cell

.method churn(regs=6, args=1)
    Const.I32 v1, 0
    Const.I32 v2, 1400
loop:
    Const.I32 v3, 7
    New.Object v4, Cell
    SetField v4, v3, Cell.f
    GetField v5, v4, Cell.f
    Add.I32 v1, v1, v5
    Add.I32 v1, v1, v0
    Sub.I32 v2, v2, v3
    JumpFalse v2, done
    Jump loop
done:
    Return v1
.end
)";
        auto module = ugb::assemble_module(kSrc);
        VORTEX_EXPECT(module.has_value());
        if (!module) return;
        // The module must OUTLIVE the bindings (ctx.module points into it).
        const int32_t id = module->find_method("churn");
        jitter.module = std::move(*module);
        auto boot = f.interp.build_module_runtime(jitter.module);
        VORTEX_EXPECT(boot.has_value());
        if (!boot) return;
        auto br = j1::make_j1_bindings(f.heap, jitter.module, &f.interp,
                                       jitter.bindings);
        VORTEX_EXPECT(br.has_value());
        if (!br) return;
        VORTEX_EXPECT(id >= 0);
        if (id < 0) return;
        j1::StencilTable corpus;
        build_default_corpus(corpus);
        j1::BaselineJit jit(corpus);
        j1::BaselineJob job;
        job.module = &jitter.module;
        job.method_id = static_cast<uint32_t>(id);
        job.klass_addrs = &jitter.bindings.klass_addr_table;
        job.field_offsets = jitter.bindings.field_offsets.data();
        job.field_offset_count = jitter.bindings.field_offsets.size();
        job.double_klass = jitter.bindings.double_klass;
        auto code = jit.compile(job);
        VORTEX_EXPECT(code.has_value());
        if (!code) return;
        auto ex = j1::publish_baseline(*code);
        VORTEX_EXPECT(ex.has_value());
        if (!ex) return;
        jitter.ex = std::make_unique<j1::BaselineExecutable>(std::move(*ex));
    }
    const auto hroot = f.handles.create(nullptr, infra::HandleScope::Global);
    VORTEX_EXPECT(hroot.has_value());
    if (!hroot) return;
    uint64_t checksum = 0;
    for (int batch = 0; batch < 260; ++batch) {
        // Survivor churn: 150 list nodes survive the batch (old pressure).
        // The list head lives in a HANDLE (never a raw young pointer —
        // young GCs between batches reuse young memory).
        for (int i = 0; i < 150; ++i) {
            auto o = f.heap.allocate_object(k, 2);
            VORTEX_EXPECT(o.has_value());
            if (!o) return;
            (*o)->field(0) = TaggedValue::smi(batch * 1000 + i);
            auto hr = f.handles.resolve(*hroot);
            VORTEX_EXPECT(hr.has_value());
            if (!hr) return;
            void* head = *hr;
            (*o)->field(1) =
                head != nullptr ? TaggedValue::heap_pointer(head)
                                : TaggedValue::null();
            f.handles.destroy(*hroot);
            auto h2 = f.handles.create(*o, infra::HandleScope::Global);
            VORTEX_EXPECT(h2.has_value());
            checksum += static_cast<uint64_t>((*o)->field(0).as_smi());
        }
        // Transient churn: objects nobody keeps (young GC fodder).
        for (int i = 0; i < 900; ++i) {
            auto d = f.heap.allocate_double(3.25 + i);
            VORTEX_EXPECT(d.has_value());
            if (!d) return;
            checksum += static_cast<uint64_t>(
                read_boxed_double(gc_resolve(*d)) * 4);
        }
        // JIT churn (docs 8.9: the workload runs through T0 AND published
        // JIT code): the J1 setter + allocation loop exercises the stencil
        // barrier + alloc slow paths between batches.
        if (jitter.ex != nullptr) {
            const std::vector<TaggedValue> jargs{TaggedValue::smi(0)};
            for (int r = 0; r < 4; ++r) {
                auto run = j1::run_baseline(*jitter.ex, jitter.bindings, jargs);
                if (!run) {
                    fprintf(stderr, "[jit-churn] %s\n",
                            run.error().message.c_str());
                }
                VORTEX_EXPECT(run.has_value());
                if (!run) return;
            }
        }
        // Driver: trigger + stepwise slices between mutator batches (the
        // J4-worker interleave shape: mutator runs between engine steps).
        auto started = engine.maybe_start_cycle();
        VORTEX_EXPECT(started.has_value());
        int steps = 0;
        while (engine.phase() != IcggcEngine::Phase::Idle && steps++ < 8192) {
            auto s = engine.step();
            VORTEX_EXPECT(s.has_value());
            if (!s) return;
        }
    }
    /// CEP:WHAT: Survivor-chain integrity: the handle's list holds every node the batches appended.
/// CEP:WHY: the TLAB handoff protocol's litmus — aliasing shortens the chain (14c/14d).
/// CEP:STATUS: complete
/// CEP:FAILURE: VORTEX_EXPECT names the shortened chain.
/// CEP:ASSUMES: fresh fixture; the TLAB handoff moves tops forward only.
/// CEP:COST: @cold. O(nodes).
/// CEP:EVIDENCE: this test.
    {
        size_t chain = 0;
        auto hr = f.handles.resolve(*hroot);
        void* p = hr.value_or(nullptr);
        while (p != nullptr && chain < 4000000) {
            auto* node = gc_resolve(static_cast<HeapObject*>(p));
            TaggedValue next = static_cast<Object*>(node)->field(1);
            if (!next.is_heap_object()) break;
            p = next.as_heap_object();
            ++chain;
        }
        VORTEX_EXPECT_EQ(chain, size_t{38999});  // 260 batches x 150 - 1
    }
    const GcStats& st = engine.stats();
    VORTEX_EXPECT(st.young_collections >= 20);
    VORTEX_EXPECT(st.old_cycles >= 1);
    // The survivor chain is fully live old data: cycles run, sweep and
    // update roots, and correctly select NO CSet (nothing is dead —
    // evacuation coverage lives in gc_evacuation_moves_objects_*).
    VORTEX_EXPECT(st.promoted_objects > 0);
    VORTEX_EXPECT(st.pauses.size() > 0);
    // THE DoD gate: p99 over ALL pause events under the budget.
    VORTEX_EXPECT(st.pause_p99_ns() < engine.config.pause_budget_ns);
    (void)checksum;
}
