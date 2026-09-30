// CEP:FILE: tests/test_j4.cpp
// CEP:WHAT: The J4 milestone test suite: determinism, fixed-point invariants, persistent IR, the stepwise worker, the tiering gate, executed OSR parity.
// CEP:WHY: Every M4 DoD item needs an executable proof; these tests are the evidence the CEP:EVIDENCE fields across the J4 tree point at.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: A failing VORTEX_EXPECT prints the file/line and fails the binary (exit 1); no test aborts silently.
// CEP:ASSUMES: the shared harness (vortex_test.hpp) registers and runs the bodies; each test builds its own heap/interpreter/module.
// CEP:COST: test-time only - a handful of compiles per test (milliseconds each) plus short T0/J3/J4 runs; no hot-path cost ships from this file.
// CEP:EVIDENCE: this file IS the M4 evidence base; see docs/tier-j4.md section 12.5 for the DoD mapping.

#include "vortex_test.hpp"

#include <memory>
#include <vector>

#include "vortex/deopt/rbpd.hpp"
#include "vortex/gc/icggc.hpp"
#include "vortex/infra/code_range.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/j3/full_jit.hpp"
#include "vortex/j4/max_jit.hpp"
#include "vortex/j4/persistent_ir.hpp"
#include "vortex/j4/worker.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/verifier.hpp"
#include "vortex/vm/interpreter.hpp"
#include "vortex/runtime/tiering.hpp"

using namespace vortex;
using namespace vortex::ugb;
using namespace vortex::deopt;
using namespace vortex::j2;
using namespace vortex::j3;
using namespace vortex::j4;

namespace {

/// CEP:WHAT: Assembles + verifies a module, failing the test with the printed named reason on refusal.
/// CEP:WHY: Every J4 test needs a valid UGB module; centralizing keeps refusals visible instead of swallowed.
/// CEP:STATUS: complete
/// CEP:FAILURE: Bumps the harness failure count and returns null on assemble/verify errors (Rule 76 wording printed).
/// CEP:ASSUMES: called from a VORTEX_TEST body (failure_count is live).
/// CEP:COST: test-time only: one assemble + one verify per call.
/// CEP:EVIDENCE: used by every test below.
std::unique_ptr<UGBModule> checked_module(const char* src) {
    auto module = assemble_module(src);
    if (!module) {
        std::printf("  [j4] assemble: %s\n", module.error().message.c_str());
        ++::vortex::testing::failure_count();
        return nullptr;
    }
    auto verified = verify_module(*module);
    if (!verified) {
        std::printf("  [j4] verify: %s\n", verified.error().message.c_str());
        ++::vortex::testing::failure_count();
        return nullptr;
    }
    return std::make_unique<UGBModule>(std::move(*module));
}

/// A hot summation loop with a direct call inside (exercises the inliner's
/// unbounded chain guard and the fixed point on a graph with a loop).
constexpr const char* kLoopCaller = R"(
.method helper(regs=4, args=1)
    Const.I64 v1, 2
    Mul.I64 v1, v0, v1
    Return v1
.end
.method main(regs=6, args=1)
    Const.I64 v1, 0
    Const.I64 v2, 1
loop:
    Const.I64 v4, 0
    Eq.I64 v5, v0, v4
    JumpTrue v5, done
    Call.Direct v3, v0, 1, helper
    Add.I64 v1, v1, v3
    Sub.I64 v0, v0, v2
    Jump loop
done:
    Return v1
.end
)";
/// CEP:WHAT: Builds a J2Job for `entry` with fresh runtime tables and profile pointers.
/// CEP:WHY: The J4 engine consumes the same job shape as J2/J3 (Rule 19); this binds the test modules to that contract.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (assumes the method exists - tests checked_module first).
/// CEP:ASSUMES: method.ensure_runtime_tables zeroes the profile/IC tables (deterministic cold state).
/// CEP:COST: test-time only: a few table resizes per call.
/// CEP:EVIDENCE: used by every tier-level test below.

J2Job make_job(UGBModule& module, const char* entry) {
    J2Job job;
    const int id = module.find_method(entry);
    job.module = &module;
    job.method_id = static_cast<uint32_t>(id);
    ugb::UGBMethod& method = module.method_table[job.method_id];
    method.ensure_runtime_tables(method.code.size() / 4 + 1);
    job.profiles = &method.profiles;
    job.ics = &method.ics;
    job.node_cap = 200'000;
    return job;
}

}  // namespace

// ---- determinism (DoD 1) ----------------------------------------------------------------

/// CEP:WHAT: Pins Rule 56: two independent compiles of the same bytecode emit identical bytes, entry/OSR offsets, region shape and summary hash.
/// CEP:WHY: The M4 DoD 1 - determinism is the observable of the no-search/no-budget engine (Rule 15/16).
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: Both modules are freshly assembled and verified; each gets an identical T0 warmup so the token cache and profiles match (the M4 review: an un-warmed module pins determinism only on a non-spliced graph — with warmup the workload inlines, so the byte compare also covers spliced code and the publish-time relocations).
/// CEP:COST: Compile-time only: two T0 warmup runs + two J4 compiles + byte-wise compares.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.5.
VORTEX_TEST(j4_determinism_bit_identical_outputs) {
    auto module_a = checked_module(kLoopCaller);
    if (!module_a) return;
    auto module_b = checked_module(kLoopCaller);
    if (!module_b) return;
    // Identical T0 warmup per module: fills profiles/ICs and the lazy
    // method-resolution cache symmetrically, so both compiles inline the
    // same calls and the byte compare covers spliced shapes too.
    for (std::unique_ptr<UGBModule>* m : {&module_a, &module_b}) {
        gc::Heap heap;
        vm::Interpreter interp(heap);
        auto boot = interp.build_module_runtime(**m);
        VORTEX_EXPECT(boot.has_value());
        if (!boot) return;
        const std::vector<TaggedValue> warm{TaggedValue::smi(4)};
        auto run = interp.run(**m, "main", warm);
        VORTEX_EXPECT(run.has_value());
        if (!run) return;
    }
    J2Job job_a = make_job(*module_a, "main");
    J2Job job_b = make_job(*module_b, "main");

    J4Stats stats_a;
    J4Stats stats_b;
    auto code_a = compile_j4(job_a, nullptr, &stats_a);
    auto code_b = compile_j4(job_b, nullptr, &stats_b);
    VORTEX_EXPECT(code_a.has_value());
    VORTEX_EXPECT(code_b.has_value());
    if (!code_a || !code_b) return;

    // Rule 56: same input -> same bytes. Bytecode identity, entry and OSR
    // offsets, region table shape: every observable of the artifact.
    VORTEX_EXPECT_EQ(code_a->code, code_b->code);
    VORTEX_EXPECT_EQ(code_a->entry_offset, code_b->entry_offset);
    VORTEX_EXPECT_EQ(code_a->osr_entry_offset, code_b->osr_entry_offset);
    VORTEX_EXPECT_EQ(code_a->regions.size(), code_b->regions.size());
    VORTEX_EXPECT_EQ(code_a->summary.graph_hash, code_b->summary.graph_hash);
}

/// CEP:WHAT: Pins the termination rule: the pipeline stops on the stability edge with pipeline_runs >= 2 and the summary bound to the final fingerprint.
/// CEP:WHY: Spec 12.2 invariants are the replacement for iteration caps - they must be executable assertions, not prose.
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: The workload converges (stable, no cycle) - a cycle would fail the stable assert and that failure is meaningful.
/// CEP:COST: Compile-time only: one J4 compile + telemetry asserts.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.5.
VORTEX_TEST(j4_fixed_point_termination_invariants) {
    auto module = checked_module(kLoopCaller);
    if (!module) return;
    J2Job job = make_job(*module, "main");
    J4Stats stats;
    auto code = compile_j4(job, nullptr, &stats);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    // The fixed point terminated on the STABILITY edge: at least one
    // confirming pipeline run observed an unchanged fingerprint. No
    // iteration cap exists (Rule 15) — the loop ran until the rule fired.
    VORTEX_EXPECT(stats.fixed_point.stable);
    VORTEX_EXPECT(!stats.fixed_point.cycle);
    VORTEX_EXPECT(stats.pipeline_runs >= 2);
    VORTEX_EXPECT(stats.fixed_point.iterations >= 1);
    VORTEX_EXPECT(stats.fixed_point.node_count > 0);
    // The summary binds to the exact graph the fixed point ended on
    // (docs/xlea.md section 4.1 Identity law).
    VORTEX_EXPECT_EQ(code->summary.graph_hash, stats.fixed_point.graph_hash);
}

// ---- persistent IR store (spec 12.1) -----------------------------------------------------

/// CEP:WHAT: Pins spec 12.1: a store-served compile reuses the graph (reused_ir) and emits bytes identical to the from-bytecode compile.
/// CEP:WHY: Persistent IR that changed the output would break replayability; the store must lie ON the pipeline trajectory.
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: The store holds exactly one entry for the single-method workload.
/// CEP:COST: Compile-time only: three J4 compiles (fresh, store-cold, store-warm).
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.5.
VORTEX_TEST(j4_persistent_ir_reuse_is_bit_identical) {
    auto module = checked_module(kLoopCaller);
    if (!module) return;
    J2Job job = make_job(*module, "main");

    PersistentIrStore store;
    J4Stats fresh_stats;
    auto fresh = compile_j4(job, nullptr, &fresh_stats);
    VORTEX_EXPECT(fresh.has_value());
    if (!fresh) return;

    // Compile #1 through the store (build + fixed point + store).
    J4Stats first_stats;
    auto first = compile_j4(job, &store, &first_stats);
    VORTEX_EXPECT(first.has_value());
    if (!first) return;
    VORTEX_EXPECT(!first_stats.reused_ir);
    VORTEX_EXPECT_EQ(store.size(), static_cast<size_t>(1));

    // Compile #2 through the store: the entry is reused, and the emitted
    // bytes are identical to the from-bytecode compile (spec 12.1: the
    // stored state lies on the pipeline's trajectory, so the fixed point
    // converges to the same artifact).
    J4Stats second_stats;
    auto second = compile_j4(job, &store, &second_stats);
    VORTEX_EXPECT(second.has_value());
    if (!second) return;
    VORTEX_EXPECT(second_stats.reused_ir);
    VORTEX_EXPECT_EQ(second->code, fresh->code);
    VORTEX_EXPECT_EQ(second->summary.graph_hash, fresh->summary.graph_hash);
    VORTEX_EXPECT_EQ(second->entry_offset, fresh->entry_offset);
}

/// CEP:WHAT: Pins identity semantics: a bytecode edit flips the version hash and drops the entry; explicit invalidation retires it; unknown keys are no-ops.
/// CEP:WHY: The store must never guess staleness (replace, never patch) and must fail closed.
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: A one-byte code edit changes method_version_hash (FNV over code bytes).
/// CEP:COST: Compile-time only: two compiles + hash flips + store probes.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.5.
VORTEX_TEST(j4_persistent_ir_invalidation_is_content_addressed) {
    auto module = checked_module(kLoopCaller);
    if (!module) return;
    J2Job job = make_job(*module, "main");
    PersistentIrStore store;
    auto first = compile_j4(job, &store, nullptr);
    VORTEX_EXPECT(first.has_value());
    if (!first) return;
    const uint64_t key =
        PersistentIrStore::store_key(*module, job.method_id);
    const uint64_t version =
        PersistentIrStore::method_version_hash(*module, job.method_id);
    VORTEX_EXPECT(store.find_reusable(key, version) != nullptr);

    // Bytecode change -> version hash mismatch -> the entry is dropped,
    // never patched (spec 12.1: the store never guesses staleness).
    ugb::UGBMethod& method = module->method_table[job.method_id];
    method.code.push_back(0x00);  // one extra padding byte changes the hash
    const uint64_t version2 =
        PersistentIrStore::method_version_hash(*module, job.method_id);
    VORTEX_EXPECT(version2 != version);
    VORTEX_EXPECT(store.find_reusable(key, version2) == nullptr);

    // Explicit invalidation (the dependency engine's path) also retires
    // the entry for the ORIGINAL version.
    auto module2 = checked_module(kLoopCaller);
    if (!module2) return;
    J2Job job2 = make_job(*module2, "main");
    PersistentIrStore store2;
    auto recompile = compile_j4(job2, &store2, nullptr);
    VORTEX_EXPECT(recompile.has_value());
    if (!recompile) return;
    const uint64_t key2 =
        PersistentIrStore::store_key(*module2, job2.method_id);
    const uint64_t version_b =
        PersistentIrStore::method_version_hash(*module2, job2.method_id);
    store2.invalidate(key2);
    VORTEX_EXPECT(store2.find_reusable(key2, version_b) == nullptr);
    // Invalidation of an unknown key is a no-op, not an error.
    store2.invalidate(key2 + 1);
}

// ---- background worker (spec 12.3) --------------------------------------------------------

/// CEP:WHAT: Pins spec 12.3: J3 executes to exact results between every worker step; publish swaps regions incrementally and the published J4 code runs to parity.
/// CEP:WHY: The "J3 keeps running during a J4 compile" DoD item - the mutator/compile isolation must be observable.
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: The workload converges quickly, so steps may be few - the stable fingerprint is asserted, not the step count.
/// CEP:COST: Compile-time + run-time: one J3 compile/publish, stepwise J4 with interleaved J3 runs, one synchronous J4 for equivalence.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.5.
VORTEX_TEST(j4_worker_stepwise_j3_keeps_running) {
    auto module = checked_module(kLoopCaller);
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto boot = interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;

    // The J3 tier keeps serving the method while the J4 worker grinds.
    J2Job j3_job = make_job(*module, "main");
    auto j3_code = compile_j3(j3_job);
    VORTEX_EXPECT(j3_code.has_value());
    if (!j3_code) return;
    J2Code j3_code_storage = std::move(*j3_code);
    const ir::EscapeSummary j3_summary = j3_code_storage.summary;
    auto j3_pub = publish_j3(j3_code_storage, j3_summary,
                             infra::global_code_range());
    VORTEX_EXPECT(j3_pub.has_value());
    if (!j3_pub) return;
    J3Executable j3_exec = std::move(*j3_pub);

    const std::vector<TaggedValue> args{TaggedValue::smi(10)};
    const int64_t kExpected = 2 * (10 + 9 + 8 + 7 + 6 + 5 + 4 + 3 + 2 + 1);

    PersistentIrStore store;
    J4Worker worker(store);
    J2Job j4_job = make_job(*module, "main");
    auto started = worker.start(j4_job);
    VORTEX_EXPECT(started.has_value());
    if (!started) return;
    VORTEX_EXPECT_EQ(worker.phase(), J4Worker::Phase::Captured);

    // Interleave: one worker step, one full J3 execution, repeat. The J3
    // results must stay exact at every step (the worker holds no shared
    // mutable state with the run path — spec 12.3).
    unsigned steps = 0;
    while (worker.phase() != J4Worker::Phase::Emitted &&
           worker.phase() != J4Worker::Phase::Published &&
           worker.phase() != J4Worker::Phase::Cancelled) {
        auto stepped = worker.step();
        VORTEX_EXPECT(stepped.has_value());
        if (!stepped) return;
        ++steps;
        auto j3_run = run_j3(j3_exec, bindings, args, interp);
        VORTEX_EXPECT(j3_run.has_value());
        if (!j3_run) return;
        VORTEX_EXPECT_EQ(j3_run->as_smi(), kExpected);
        if (steps > 1000) {
            // The termination rule guarantees progress; this guards the
            // TEST against a runaway loop masking a regression.
            VORTEX_EXPECT(false && "worker did not terminate");
            return;
        }
    }
    VORTEX_EXPECT(steps >= 1);  // the emit step; the fixed point may have
    // converged during capture's first pipeline run (deterministic input)
    VORTEX_EXPECT(worker.last_state().stable);

    // Publish (with the live J3 region table -> incremental replacement),
    // then EXECUTE the worker-published J4 executable: parity with J3
    // (Rule 18) — the publication is proven, not just tolerated.
    const size_t j3_regions_before = j3_exec.core.regions->size();
    auto published = worker.publish(j3_exec.core.regions.get());
    VORTEX_EXPECT(published.has_value());
    if (!published) return;
    VORTEX_EXPECT_EQ(worker.phase(), J4Worker::Phase::Published);
    const size_t j4_regions_fresh = published->regions->size();
    VORTEX_EXPECT(j4_regions_fresh >= 1);
    j2::J2Executable j4_exec = std::move(*published);
    // The worker's state was returned to the store as Published (spec
    // 12.1's lifecycle states are observable).
    VORTEX_EXPECT_EQ(store.size(), static_cast<size_t>(1));
    VORTEX_EXPECT(store.find_reusable(
                      PersistentIrStore::store_key(*module, j4_job.method_id),
                      PersistentIrStore::method_version_hash(
                          *module, j4_job.method_id)) != nullptr);

    // Post-publication region shape: the live table grew by at most the
    // fresh table's size, every fresh region landed somewhere (replaced a
    // live overlap -> that old id went stale; or appended), and the count
    // of stale entries equals the number of replacements.
    const auto& live = *j3_exec.core.regions;
    size_t stale_now = 0;
    for (const auto& r : live.regions()) {
        if (r.state == deopt::RegionState::Stale) ++stale_now;
    }
    VORTEX_EXPECT(live.size() >= j3_regions_before);
    VORTEX_EXPECT(live.size() <= j3_regions_before + j4_regions_fresh);
    VORTEX_EXPECT(stale_now >= 1);  // at least the entry region was replaced

    // Execute the PUBLISHED artifact (not a separate compile).
    {
        TaggedValue ret = TaggedValue::undefined();
        heap.sync_tlab_top(static_cast<uint8_t*>(bindings.context.tlab_top));
        const int64_t rc =
            j4_exec.entry(&bindings.context, args.data(),
                          static_cast<uint32_t>(args.size()), &ret);
        VORTEX_EXPECT_EQ(rc, 0);
        if (rc == 0) VORTEX_EXPECT_EQ(ret.as_smi(), kExpected);
    }

    // A fresh synchronous J4 compile must equal the worker's outcome
    // (same deterministic engine, one loop implementation).
    J4Stats sync_stats;
    auto sync_code = compile_j4(j4_job, nullptr, &sync_stats);
    VORTEX_EXPECT(sync_code.has_value());
    if (!sync_code) return;
    VORTEX_EXPECT(sync_stats.fixed_point.stable);
    auto sync_pub = publish_j2(*sync_code, infra::global_code_range());
    VORTEX_EXPECT(sync_pub.has_value());
    if (!sync_pub) return;
    j2::J2Executable sync_exec = std::move(*sync_pub);
    TaggedValue ret = TaggedValue::undefined();
    heap.sync_tlab_top(static_cast<uint8_t*>(bindings.context.tlab_top));
    const int64_t rc = sync_exec.entry(&bindings.context, args.data(),
                                       static_cast<uint32_t>(args.size()),
                                       &ret);
    VORTEX_EXPECT_EQ(rc, 0);
    if (rc == 0) VORTEX_EXPECT_EQ(ret.as_smi(), kExpected);
}

/// CEP:WHAT: Pins Rule 15 cancellation: invalidate() defers to the next step boundary, lands in Cancelled with Event::Cancelled, and phase misuse is a named refusal.
/// CEP:WHY: Cancellation must never wait on the compile and must be observable by name (Rules 76/28).
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: The worker is one-shot: a fresh compile needs a fresh worker.
/// CEP:COST: Compile-time only: one capture + cancel + terminal-phase probes.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.5.
VORTEX_TEST(j4_worker_cancellation_is_named_and_deferred) {
    auto module = checked_module(kLoopCaller);
    if (!module) return;
    PersistentIrStore store;
    J4Worker worker(store);
    J2Job job = make_job(*module, "main");
    auto started = worker.start(job);
    VORTEX_EXPECT(started.has_value());
    if (!started) return;

    // External invalidation (Rule 15: cancellable): the draft dies at the
    // NEXT step boundary, by name.
    worker.invalidate();
    auto stepped = worker.step();
    VORTEX_EXPECT(stepped.has_value());
    if (!stepped) return;
    VORTEX_EXPECT_EQ(*stepped, J4Worker::Phase::Cancelled);
    VORTEX_EXPECT_EQ(worker.last_event(), J4Worker::Event::Cancelled);

    // Stepping a terminal phase is a no-op, not an error; starting a fresh
    // compile requires a fresh worker (one-shot draft lifecycle).
    auto again = worker.step();
    VORTEX_EXPECT(again.has_value());
    if (again) VORTEX_EXPECT_EQ(*again, J4Worker::Phase::Cancelled);

    // Phase misuse is a named refusal (Rule 76).
    J4Worker idle_worker(store);
    auto bad_step = idle_worker.step();
    VORTEX_EXPECT(!bad_step.has_value());
    auto bad_publish = idle_worker.publish(nullptr);
    VORTEX_EXPECT(!bad_publish.has_value());
}

// The policy test asserts throttle_verdict's ordinal directly; this pins
// the mirror against silent enum reordering (CEP&CC 11.3: contracts get
// static assertions, not prose).
static_assert(static_cast<int>(deopt::EntryDecision::RefuseMethod) == 3,
              "throttle_verdict mirrors the EntryDecision ladder ordinals");

// ---- tiering driver: Rule-43 verdict consumption (spec 12.4) -------------------------------

namespace {

/// A region table past the method throttle threshold; built by
/// make_blacklisted_table (a constructor here would sit between the
/// struct's CEP block and its first member, which defeats the linter's
/// function-block association).
struct BlacklistedTable {
    deopt::RegionTable table;
    deopt::ThrottlePolicy policy;
};

/// CEP:WHAT: Builds a region table driven past the method throttle threshold (chronic failure).
/// CEP:WHY: The tiering-gate tests need a table whose entry_check refuses with RefuseMethod - the Rule-43 ladder end state.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (deterministic failure injection via on_guard_failure).
/// CEP:ASSUMES: ThrottlePolicy defaults (method_threshold 64); ticks are synthetic counters (the engine stays clock-free).
/// CEP:COST: test-time only: 64 counter increments per call.
/// CEP:EVIDENCE: j4_tiering_policy_consumes_rbpd_verdict, j4_tiering_gate_blocks_re_promotion.
BlacklistedTable make_blacklisted_table() {
    BlacklistedTable blacklist;
    deopt::RegionDescriptor d;
    d.kind = deopt::RegionKind::Hot;
    d.bytecode_pc_begin = 0;
    d.bytecode_pc_end = 64;
    (void)blacklist.table.add_region(d);
    uint64_t tick = 0;
    for (uint32_t i = 0; i < blacklist.policy.method_threshold + 1; ++i) {
        (void)blacklist.table.on_guard_failure(0, blacklist.policy, 0,
                                               deopt::DeoptReason::GuardFailed,
                                               ++tick);
    }
    return blacklist;
}

constexpr const char* kTinyLoop = R"(
.method main(regs=4, args=1)
    Const.I64 v1, 1
loop:
    Const.I64 v2, 0
    Eq.I64 v3, v0, v2
    JumpTrue v3, done
    Sub.I64 v0, v0, v1
    Jump loop
done:
    Return v0
.end
)";

// Mutual recursion: even -> odd -> even is a genuine call CYCLE (spec
// 12.2's termination invariant exercises the call-graph cycle guard, not
// just depth). even(6) = 1; even(7) = 0.
constexpr const char* kMutualRecursion = R"(
.method even(regs=6, args=1)
    Const.I64 v2, 0
    Eq.I64 v3, v0, v2
    JumpTrue v3, even_base
    Const.I64 v1, 1
    Sub.I64 v0, v0, v1
    Call.Direct v4, v0, 1, odd
    Return v4
even_base:
    Const.I64 v4, 1
    Return v4
.end
.method odd(regs=6, args=1)
    Const.I64 v2, 0
    Eq.I64 v3, v0, v2
    JumpTrue v3, odd_base
    Const.I64 v1, 1
    Sub.I64 v0, v0, v1
    Call.Direct v4, v0, 1, even
    Return v4
odd_base:
    Const.I64 v4, 0
    Return v4
.end
.method main(regs=4, args=1)
    Call.Direct v1, v0, 1, even
    Return v1
.end
)";

}  // namespace

/// CEP:WHAT: Pins spec 12.4 at the policy level: refused verdicts block escalations, demote optimized tiers to T0, and leave J1 reachable.
/// CEP:WHY: The Rule-43 consumption point must be a pure function of hotness AND verdict so any driver agrees.
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: The graduated-ladder defaults are used (Rule 72 knobs).
/// CEP:COST: Test-time only: policy evaluations on synthetic hotness.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.5.
VORTEX_TEST(j4_tiering_policy_consumes_rbpd_verdict) {
    BlacklistedTable blacklist = make_blacklisted_table();
    VORTEX_EXPECT_EQ(blacklist.table.throttle_verdict(blacklist.policy), 3);
    VORTEX_EXPECT(blacklist.table.entry_check(blacklist.policy) ==
                  deopt::EntryDecision::RefuseMethod);

    TieringThresholds thresholds;  // defaults: the graduation ladder
    TieringPolicy policy(thresholds);
    const deopt::EntryDecision refused = deopt::EntryDecision::RefuseMethod;
    const deopt::EntryDecision weaken = deopt::EntryDecision::WeakenAssumptions;

    // Hot enough for every rung; the current tier varies.
    MethodHotness hot;
    hot.invocations = thresholds.j4_invocations * 10;
    hot.backedges = thresholds.j4_invocations * 10;

    // Healthy verdict: the ladder escalates normally.
    MethodHotness h0 = hot;
    h0.current = Tier::J1;
    VORTEX_EXPECT(policy.evaluate(h0) == Tier::J2);

    // WeakenAssumptions is NOT a refusal: escalation stays legal (the run
    // path schedules the weaker recompile — docs/deopt-rbpd.md 9).
    VORTEX_EXPECT(policy.evaluate(h0, weaken) == Tier::J2);

    // Refused escalation: a gated method does NOT move up the optimizing
    // ladder (any current tier below J2 stays put).
    VORTEX_EXPECT(policy.evaluate(h0, refused) == Tier::J1);
    MethodHotness h1 = hot;
    h1.current = Tier::T0;
    VORTEX_EXPECT(policy.evaluate(h1, refused) == Tier::J1);

    // J1 is non-speculative: the stencil stays reachable even when gated
    // (Rule 43 throttles SPECULATION, not compilation).
    MethodHotness cold;
    cold.invocations = thresholds.j1_invocations;
    cold.current = Tier::T0;
    VORTEX_EXPECT(policy.evaluate(cold, refused) == Tier::J1);

    // A method already on an optimized tier falls back to T0 (Rule 40).
    MethodHotness h3 = hot;
    h3.current = Tier::J3;
    VORTEX_EXPECT(policy.evaluate(h3, refused) == Tier::T0);
    MethodHotness h4 = hot;
    h4.current = Tier::J4;
    VORTEX_EXPECT(policy.evaluate(h4, refused) == Tier::T0);

    // DowngradeTier refuses exactly like RefuseMethod (the ladder mirrors
    // throttle_verdict's numbers — docs/deopt-rbpd.md 9).
    VORTEX_EXPECT(policy.evaluate(h3, deopt::EntryDecision::DowngradeTier) ==
                  Tier::T0);
}

/// CEP:WHAT: Pins the driver wiring: speculation_verdict reflects the installed gate, execution continues on T0/J1 while gated, and clearing ends the block.
/// CEP:WHY: The trap gates speculation, never execution (Rules 40/43) - the method must stay runnable while blacklisted.
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: Tiny tiering thresholds make a handful of invocations cross the promotion rungs.
/// CEP:COST: Run-time: 16 short T0 runs across the gate install/clear boundary.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.5.
VORTEX_TEST(j4_tiering_gate_blocks_re_promotion) {
    BlacklistedTable blacklist = make_blacklisted_table();
    auto module = checked_module(kTinyLoop);
    if (!module) return;
    gc::Heap heap;
    vm::InterpreterConfig config;
    // Tiny thresholds so a handful of invocations crosses them (Rule 72:
    // thresholds are named, configurable knobs).
    config.tiering.j1_invocations = 2;
    config.tiering.j1_backedges = 2;
    config.tiering.j2_invocations = 4;
    config.tiering.j2_backedges = 4;
    vm::Interpreter interp(heap, config);
    auto boot = interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    const int32_t id = module->find_method("main");
    VORTEX_EXPECT(id >= 0);
    if (id < 0) return;

    // Without a gate: the verdict is Enter and hot loops promote freely.
    VORTEX_EXPECT(interp.speculation_verdict(static_cast<uint32_t>(id)) ==
                  deopt::EntryDecision::Enter);
    const std::vector<TaggedValue> args{TaggedValue::smi(3)};
    for (int i = 0; i < 8; ++i) {
        auto run = interp.run(*module, "main", args);
        VORTEX_EXPECT(run.has_value());
        if (!run) return;
    }
    VORTEX_EXPECT(!interp.tier_transitions().empty());

    // With the gate: the verdict the driver (and the policy) see is the
    // named refusal; the method keeps EXECUTING on T0/J1 throughout —
    // the trap gates speculation, never execution (Rules 40/43).
    interp.set_speculation_gate(static_cast<uint32_t>(id), &blacklist.table,
                                blacklist.policy);
    VORTEX_EXPECT(interp.speculation_verdict(static_cast<uint32_t>(id)) ==
                  deopt::EntryDecision::RefuseMethod);
    const size_t gated_base = interp.tier_transitions().size();
    for (int i = 0; i < 8; ++i) {
        auto run = interp.run(*module, "main", args);
        VORTEX_EXPECT(run.has_value());
        if (!run) return;
        VORTEX_EXPECT_EQ(run->value.as_smi(), 0);
    }

    // The verdict rode INTO the policy through record_backedge (the
    // interpreter passes speculation_verdict(id) as evaluate's second
    // argument — the M4 review's untested-glue finding): no gated-phase
    // transition reaches an optimizing tier. J1 stays reachable (Rule 43
    // gates speculation, not compilation). Scope note: the interpreter's
    // backedge view is deliberately T0-only (record_backedge hardcodes
    // h.current = T0; the external tiering driver owns real currents), so
    // evaluate's T0-view ladder caps at J1 and this assertion pins the
    // gate-side contract that is observable here — the J1/J2+ escalation
    // refusals for non-T0 currents are pinned at the policy level
    // (j4_tiering_policy_consumes_rbpd_verdict).
    const std::span<const vm::TierTransitionRecord> transitions =
        interp.tier_transitions();
    for (size_t k = gated_base; k < transitions.size(); ++k) {
        VORTEX_EXPECT(transitions[k].to != Tier::J2);
        VORTEX_EXPECT(transitions[k].to != Tier::J3);
        VORTEX_EXPECT(transitions[k].to != Tier::J4);
    }

    // Clearing the gate ends the block (the caller owns expiry).
    interp.clear_speculation_gate(static_cast<uint32_t>(id));
    VORTEX_EXPECT(interp.speculation_verdict(static_cast<uint32_t>(id)) ==
                  deopt::EntryDecision::Enter);
}

// ---- executed OSR entry parity (Rule 18; roadmap M3 carry-over) ----------------------------

namespace {

constexpr const char* kOsrLoop = R"(
.method main(regs=6, args=1)
    Const.I64 v1, 0
    Const.I64 v2, 1
loop:
    Const.I64 v3, 0
    Eq.I64 v4, v0, v3
    JumpTrue v4, done
    Add.I64 v1, v1, v0
    Sub.I64 v0, v0, v2
    Jump loop
done:
    Return v1
.end
)";

struct OsrSnapshot {
    uint32_t method_id = 0xFFFFFFFF;
    uint32_t pc = 0;
    std::vector<TaggedValue> regs;
    uint32_t capture_index = 0;   // the Nth backedge we want
    uint32_t captured_count = 0;  // how many backedges were seen
};
/// CEP:WHAT: The backedge hook that copies the Nth mid-loop T0 register file into an OsrSnapshot.
/// CEP:WHY: The executed-OSR test needs a REAL snapshot from a live interpreter run, not a hand-built state (the M4 DoD).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (records what it observes; assertions live in the test).
/// CEP:ASSUMES: the hook contract (docs/tier-t0.md): copy and return quickly, never reenter the engine.
/// CEP:COST: test-time only: one vector assign per backedge observation.
/// CEP:EVIDENCE: m4_osr_entry_parity_with_t0_snapshot.

void osr_capture_hook(void* user, uint32_t method_id, uint32_t pc,
                      const TaggedValue* regs, uint32_t reg_count) {
    auto* snap = static_cast<OsrSnapshot*>(user);
    ++snap->captured_count;
    if (snap->captured_count - 1 != snap->capture_index) return;
    snap->method_id = method_id;
    snap->pc = pc;
    snap->regs.assign(regs, regs + reg_count);
}

}  // namespace

/// CEP:WHAT: Pins Rule 18 executed-OSR parity: a REAL T0 mid-loop snapshot (captured via the backedge hook) continues in the compiled OSR entry to the same result.
/// CEP:WHY: The M3 carry-over DoD: the M2 test only pinned the stub existence; M4 executes it with a live snapshot.
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: The snapshot lands on a loop head (the backedge target), so the captured register file is the OSR entry state.
/// CEP:COST: Run-time: one T0 run to completion, one hooked T0 run, one J2 compile + publish + OSR entry call.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.5.
VORTEX_TEST(m4_osr_entry_parity_with_t0_snapshot) {
    auto module = checked_module(kOsrLoop);
    if (!module) return;
    const int64_t kN = 40;
    const int64_t kExpected = 40 + 39 + 38 + 37 + 36 + 35 + 34 + 33 + 32 + 31 +
                              30 + 29 + 28 + 27 + 26 + 25 + 24 + 23 + 22 + 21 +
                              20 + 19 + 18 + 17 + 16 + 15 + 14 + 13 + 12 + 11 +
                              10 + 9 + 8 + 7 + 6 + 5 + 4 + 3 + 2 + 1;
    const std::vector<TaggedValue> args{TaggedValue::smi(kN)};

    // 1. The reference: a pure T0 run to completion.
    int64_t t0_result = 0;
    {
        gc::Heap heap;
        vm::Interpreter interp(heap);
        auto run = interp.run(*module, "main", args);
        VORTEX_EXPECT(run.has_value());
        if (!run) return;
        t0_result = run->value.as_smi();
    }
    VORTEX_EXPECT_EQ(t0_result, kExpected);

    // 2. A REAL mid-loop T0 snapshot: the backedge hook observes the live
    // register file during an actual interpreter run (the M2 test only
    // pinned the stub's existence — this executes it, roadmap M3/M4).
    OsrSnapshot snap;
    snap.capture_index = 5;  // mid-loop: 6th backedge of a 40-iteration loop
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.set_backedge_hook(&osr_capture_hook, &snap);
    {
        auto run = interp.run(*module, "main", args);
        VORTEX_EXPECT(run.has_value());
        if (!run) return;
    }
    VORTEX_EXPECT(snap.captured_count > 5);
    VORTEX_EXPECT_EQ(snap.method_id, 0u);  // method 0 (main)
    VORTEX_EXPECT_EQ(snap.regs.size(), static_cast<size_t>(6));
    // The snapshot state is the loop-head entry state: v0 (the countdown
    // register) already holds the decremented value; v1 (the accumulator)
    // holds the six adds done so far (40+39+38+37+36+35).
    VORTEX_EXPECT_EQ(snap.regs[0].as_smi(), kN - 6);
    VORTEX_EXPECT_EQ(snap.regs[1].as_smi(),
                     kN + (kN - 1) + (kN - 2) + (kN - 3) + (kN - 4) +
                         (kN - 5));

    // 3. Execute the compiled OSR entry with the captured state: parity
    // with the pure T0 result (Rule 18 — one semantic contract).
    J2Job job = make_job(*module, "main");
    auto code = compile_j2(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    VORTEX_EXPECT(code->osr_entry_offset != 0xFFFFFFFFu);
    auto ex = publish_j2(*code, infra::global_code_range());
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    VORTEX_EXPECT(ex->osr_entry != nullptr);
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    TaggedValue ret = TaggedValue::undefined();
    heap.sync_tlab_top(static_cast<uint8_t*>(bindings.context.tlab_top));
    const int64_t rc = ex->osr_entry(&bindings.context, snap.regs.data(),
                                     static_cast<uint32_t>(snap.regs.size()),
                                     &ret);
    VORTEX_EXPECT_EQ(rc, 0);
    if (rc == 0) VORTEX_EXPECT_EQ(ret.as_smi(), kExpected);
}

// ---- M4 review blocker 3: the deopt-frame ABI capacity is REAL ---------------

/// CEP:WHAT: Pins the record-side deopt-frame ABI contract: a guard spliced at the deepest inlinable depth yields a record carrying the FULL chain (root + 7 links = kMaxDeoptFrames frames).
/// CEP:WHY: M4 review blocker 3 - the capacity must be real, never silently truncated (Rules 39/42/113) and never one frame short (a conservative cap would hide representable chains).
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: kMaxDeoptFrames = 8; Div.S.I64 is guard-bearing with a materializable FrameState chain; the runtime token cache is warm before compile (see warmup note).
/// CEP:COST: Compile-time: two J4 pipeline runs over an 11-method module; run-time: one T0 warmup pass and one published-entry execution.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.2 (inline bound = the deopt-frame ABI capacity).
// The chain module keeps a d8..d10 tail of plain
// pass-through calls so the spliced region is genuinely deeper than the
// guard and the executed result crosses both compiled and runtime call
// forms. Rule 121: every regression gets a regression test.
//
// Construction note (why the guard sits in d7, not d10): a deopt record's
// frame chain only observes guards INSIDE the spliced region. With the
// guard at depth 10, the ABI cap (9 links > 8 frames) always refuses the
// splice before the guard ever enters the graph, and max_frames is 0 no
// matter how correct the engine is — the companion test below pins that
// refusal side.
VORTEX_TEST(j4_inline_chain_respects_deopt_frame_capacity) {
    // d7 divides (Div = a guard-bearing operation with a materializable
    // FrameState chain); d1..d6 forward, d8..d10 are the runtime-call tail.
    std::string src = ".method d10(regs=4, args=1)\n"
                      "  Return v0\n.end\n";
    for (int i = 9; i >= 8; --i) {
        src += ".method d" + std::to_string(i) + "(regs=4, args=1)\n";
        src += "  Call.Direct v1, v0, 1, d" + std::to_string(i + 1) + "\n";
        src += "  Return v1\n.end\n";
    }
    src += ".method d7(regs=4, args=1)\n  Const.I64 v2, 2\n"
           "  Div.S.I64 v1, v0, v2\n  Return v1\n.end\n";
    for (int i = 6; i >= 1; --i) {
        src += ".method d" + std::to_string(i) + "(regs=4, args=1)\n";
        src += "  Call.Direct v1, v0, 1, d" + std::to_string(i + 1) + "\n";
        src += "  Return v1\n.end\n";
    }
    src += ".method main(regs=4, args=1)\n  Call.Direct v1, v0, 1, d1\n"
           "  Return v1\n.end\n";
    auto module = checked_module(src.c_str());
    if (!module) return;

    // T0 warmup BEFORE the compile: the inliner resolves direct callees
    // through the runtime's lazy token cache (interpreter.cpp
    // resolve_method), which a cold module leaves empty — test_j2's
    // harness documents the same contract ("T0 warmup: fills profiles/ICs
    // and the method resolution cache"). One T0 run walks the whole chain,
    // so every d1..d10 token is resolved when compile_j4 runs.
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto boot = interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    const std::vector<TaggedValue> warm_args{TaggedValue::smi(20)};
    auto warm = interp.run(*module, "main", warm_args);
    VORTEX_EXPECT(warm.has_value());
    if (warm) VORTEX_EXPECT_EQ(warm->value.as_smi(), 10);

    // J4: the unbounded-tier config. Splicing must reach d7 (chain of 8 =
    // the exact ABI capacity) so the Div's guard carries the full chain.
    J2Job j4 = make_job(*module, "main");
    J4Stats stats;
    auto code = compile_j4(j4, nullptr, &stats);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    size_t max_frames = 0;
    for (const DeoptRecord& rec : *code->records) {
        VORTEX_EXPECT(rec.frames.size() <= j2::kMaxDeoptFrames);
        max_frames = std::max(max_frames, rec.frames.size());
    }
    VORTEX_EXPECT(max_frames == j2::kMaxDeoptFrames);  // built TO the cap

    // Plain execution parity through the REAL published stubs: the compiled
    // head (main + d1..d7 spliced, guard never fires on 20/2) hands off to
    // the runtime-call tail (d8 -> d9 -> d10) and returns 10.
    auto ex = publish_j2(*code, infra::global_code_range());
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    TaggedValue ret = TaggedValue::undefined();
    heap.sync_tlab_top(static_cast<uint8_t*>(bindings.context.tlab_top));
    const std::vector<TaggedValue> args{TaggedValue::smi(20)};
    const int64_t rc = ex->entry(&bindings.context, args.data(),
                                 static_cast<uint32_t>(args.size()), &ret);
    VORTEX_EXPECT_EQ(rc, 0);
    if (rc == 0) VORTEX_EXPECT_EQ(ret.as_smi(), 10);
}

/// CEP:WHAT: Pins the refusal-side deopt-frame ABI contract: with the guard beyond the inlinable depth, the chain-depth refusal stops splicing BEFORE the guard enters the graph and the compile still succeeds.
/// CEP:WHY: A broken (permissive) cap would splice the depth-10 guard and trip the record builder's named hard refusal instead - this test keeps the graceful inliner path the only enforcement point.
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary.
/// CEP:ASSUMES: The record builder's "deopt frame chain exceeds kMaxDeoptFrames" refusal fires if a spliced guard ever exceeds the capacity; runtime token cache warm before compile.
/// CEP:COST: Compile-time: two J4 pipeline runs over an 11-method module; run-time: one T0 warmup pass and one published-entry execution.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.2.
VORTEX_TEST(j4_inline_chain_beyond_capacity_refuses_before_record_builder) {
    std::string src = ".method d10(regs=4, args=1)\n  Const.I64 v2, 2\n"
                      "  Div.S.I64 v1, v0, v2\n  Return v1\n.end\n";
    for (int i = 9; i >= 1; --i) {
        src += ".method d" + std::to_string(i) + "(regs=4, args=1)\n";
        src += "  Const.I64 v2, 2\n";
        src += "  Call.Direct v1, v0, 1, d" + std::to_string(i + 1) + "\n";
        src += "  Return v1\n.end\n";
    }
    src += ".method main(regs=4, args=1)\n  Const.I64 v2, 2\n";
    src += "  Call.Direct v1, v0, 1, d1\n  Return v1\n.end\n";
    auto module = checked_module(src.c_str());
    if (!module) return;

    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto boot = interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    const std::vector<TaggedValue> warm_args{TaggedValue::smi(20)};
    auto warm = interp.run(*module, "main", warm_args);
    VORTEX_EXPECT(warm.has_value());
    if (warm) VORTEX_EXPECT_EQ(warm->value.as_smi(), 10);

    // The compile must SUCCEED: the inliner's capacity refusal is the
    // graceful path; the record builder's hard error is never reached.
    J2Job j4 = make_job(*module, "main");
    auto code = compile_j4(j4, nullptr, nullptr);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    // The d10 guard never entered the graph: no record can exceed the
    // capacity because none carries the refused chain.
    for (const DeoptRecord& rec : *code->records) {
        VORTEX_EXPECT(rec.frames.size() <= j2::kMaxDeoptFrames);
    }

    auto ex = publish_j2(*code, infra::global_code_range());
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    // 20 reaches d10 through the un-spliced tail: 20/2 = 10.
    TaggedValue ret = TaggedValue::undefined();
    heap.sync_tlab_top(static_cast<uint8_t*>(bindings.context.tlab_top));
    const std::vector<TaggedValue> args{TaggedValue::smi(20)};
    const int64_t rc = ex->entry(&bindings.context, args.data(),
                                 static_cast<uint32_t>(args.size()), &ret);
    VORTEX_EXPECT_EQ(rc, 0);
    if (rc == 0) VORTEX_EXPECT_EQ(ret.as_smi(), 10);
}

/// CEP:WHAT: Pins the call-graph cycle guard's termination edge on a genuinely cyclic module: mutual recursion (even -> odd -> even) compiles deterministically, terminates on the stability edge, and executes to parity.
/// CEP:WHY: The roadmap DoD names the cycle guard as the inline-growth terminator; the M4 review found no test built a cyclic module (the guard regressing would hang/OOM the unbounded J4 budget instead of failing an assertion). The executed-parity leg is ALSO the Rule-121 regression test for the JIT call path: forward-referenced callees (the cycle) exposed helper_invoke_token's positional method_table[token-1] resolution silently calling the WRONG method - the fix resolves by name like T0 (Rule 18).
/// CEP:STATUS: complete
/// CEP:FAILURE: Failing expectations print file/line and fail the binary; a guard regression manifests as a compile that never returns (CI timeout = the failing signal for a termination claim).
/// CEP:ASSUMES: The mutual-recursion module's tokens resolve through the T0 warmup (one run walks the whole cycle); the cycle guard stops re-splicing once a method repeats on the chain, so the fixed point stabilizes; even/odd bodies are multi-block (branching), so NOTHING splices here - the compiled head is one runtime-resolved call and the tier stack must agree with T0 on the answer.
/// CEP:COST: Compile-time: two J4 compiles of a 3-method cyclic module + one T0 warmup run; run-time: two published-entry executions.
/// CEP:EVIDENCE: this test; docs/tier-j4.md section 12.2 (termination invariants); docs/roadmap.md M4 DoD (inline growth terminates on the call-graph cycle guard).
VORTEX_TEST(j4_inline_cycle_guard_terminates) {
    auto module = checked_module(kMutualRecursion);
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto boot = interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    // One T0 run walks even(6) -> odd(5) -> ... -> even(0): every call
    // token in the cycle resolves, and the warm result pins T0 parity.
    const std::vector<TaggedValue> warm_args{TaggedValue::smi(6)};
    auto warm = interp.run(*module, "main", warm_args);
    VORTEX_EXPECT(warm.has_value());
    if (warm) VORTEX_EXPECT_EQ(warm->value.as_smi(), 1);

    // Two compiles of the cyclic module: the fixed point must terminate
    // (stability edge) and produce identical bytes (Rule 56 holds on
    // cyclic call graphs, not just acyclic ones).
    J2Job job_a = make_job(*module, "main");
    J2Job job_b = make_job(*module, "main");
    J4Stats stats;
    auto code_a = compile_j4(job_a, nullptr, &stats);
    auto code_b = compile_j4(job_b, nullptr, nullptr);
    VORTEX_EXPECT(code_a.has_value());
    VORTEX_EXPECT(code_b.has_value());
    if (!code_a || !code_b) return;
    VORTEX_EXPECT(stats.fixed_point.stable || stats.fixed_point.cycle);
    VORTEX_EXPECT_EQ(code_a->code, code_b->code);
    VORTEX_EXPECT_EQ(code_a->entry_offset, code_b->entry_offset);
    VORTEX_EXPECT_EQ(code_a->summary.graph_hash, code_b->summary.graph_hash);

    // Executed parity through the REAL published stub: the compiled head
    // (spliced up to the cycle guard) hands off to runtime-resolved calls
    // for the cyclic tail.
    auto ex = publish_j2(*code_a, infra::global_code_range());
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    for (const auto [input, expected] :
         {std::pair<int64_t, int64_t>{6, 1}, {7, 0}}) {
        TaggedValue ret = TaggedValue::undefined();
        heap.sync_tlab_top(static_cast<uint8_t*>(bindings.context.tlab_top));
        const std::vector<TaggedValue> args{TaggedValue::smi(input)};
        const int64_t rc = ex->entry(&bindings.context, args.data(),
                                     static_cast<uint32_t>(args.size()),
                                     &ret);
        VORTEX_EXPECT_EQ(rc, 0);
        if (rc == 0) VORTEX_EXPECT_EQ(ret.as_smi(), expected);
    }
}
