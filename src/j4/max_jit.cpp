// CEP:FILE: src/j4/max_jit.cpp
// CEP:WHAT: The J4 deterministic fixed-point engine: capture (build or persistent-IR reuse), the shared Session loop, backend emission.
// CEP:WHY: M4's core deliverable - peak tier code quality under Rule 15 (no budget) and Rule 16 (no search), with the fixed point as the only termination authority.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Named Result failures (unknown method, build refusal); the fixed point terminates on stability or a repeated hash; no exceptions, no iteration caps.
// CEP:ASSUMES: the J3 driver is deterministic and monotone-or-neutral per stage (Rules 55/56); the deopt-frame ABI capacity bounds inline depth (j2::kMaxDeoptFrames).
// CEP:COST: @cold. Build O(bytecode) + pipeline_runs x O(pipeline) + one O(live nodes) fingerprint per run + one backend emit. Real bodies converge in 2 pipeline runs (the confirming run observes an unchanged fingerprint).
// CEP:EVIDENCE: tests/test_j4.cpp - j4_determinism_bit_identical_outputs, j4_fixed_point_termination_invariants, j4_persistent_ir_reuse_is_bit_identical; docs/tier-j4.md section 12.2.
#include "vortex/j4/max_jit.hpp"

#include <cstdint>

#include "engine.hpp"
#include "vortex/j3/full_jit.hpp"
#include "vortex/j3/passes.hpp"
#include "vortex/j4/persistent_ir.hpp"
#include "vortex/support/containers.hpp"
#include "vortex/support/hash.hpp"

namespace vortex::j4 {

/// CEP:WHAT: The fixed-point fingerprint: canonical graph hash + live node count.
/// CEP:WHY: Termination invariant 1 (spec 12.2) - a pure identity of the compiled graph shared with the escape-summary Identity hash.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure function).
/// CEP:ASSUMES: j3::graph_hash is the one canonical construction for both consumers.
/// CEP:COST: @cold. One O(live nodes) FNV walk + one live_count() per call; called once per fixed-point iteration.
/// CEP:EVIDENCE: j4_fixed_point_termination_invariants (summary hash == final fingerprint).
FixedPointState fingerprint(const ir::Graph& graph) {
    FixedPointState state;
    // The canonical graph hash (j3::graph_hash — ONE construction for the
    // escape-summary Identity binding and this fingerprint).
    state.graph_hash = j3::graph_hash(graph);
    state.node_count = graph.live_count();
    return state;
}

namespace {

/// CEP:WHAT: The J4 budget: caps removed (Rule 15) except the deopt-frame ABI capacity and the Rule-59/131 kill switches.
/// CEP:WHY: "No artificial budget" must be structural - node/site/callee caps become unlimited, and the only inline depth bound left is the deopt record's frame capacity (a Rule-15 safety constraint).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure function).
/// CEP:ASSUMES: the inliner's call-graph cycle guard is always on (the termination invariant that replaces the caps).
/// CEP:COST: @cold. Seven constant assignments per compile.
/// CEP:EVIDENCE: j4_fixed_point_termination_invariants (converges without any cap firing).
j3::J3Budget j4_budget(const J2Job& job) {
    j3::J3Budget budget;
    budget.node_cap = SIZE_MAX;  // no node budget
    budget.inline_depth_cap =
        static_cast<uint32_t>(j2::kMaxDeoptFrames - 1);  // ABI capacity
    budget.inline_site_cap = UINT32_MAX;                 // no site budget
    budget.inline_callee_node_cap = SIZE_MAX;            // no callee budget
    budget.pass_control = ~job.pass_kill_switches;
    return budget;
}

}  // namespace

namespace detail {

/// CEP:WHAT: Session::capture - build or reuse the graph state (spec 12.1) and run the first pipeline iteration.
/// CEP:WHY: The capture phase brings the graph onto the pipeline's deterministic trajectory; a reused store entry usually confirms stability immediately afterwards.
/// CEP:STATUS: complete
/// CEP:FAILURE: Named Result failure on unknown method id or build refusal (Rule 76); every other path returns success.
/// CEP:ASSUMES: job.module and its runtime tables outlive the session; store may be null.
/// CEP:COST: @cold. O(bytecode) build (or O(1) reuse) + one full pipeline run; the dominant cost of iteration 1.
/// CEP:EVIDENCE: j4_persistent_ir_reuse_is_bit_identical (reused_ir flips true on the second store compile).
support::Result<void> Session::capture(const J2Job& job,
                                       PersistentIrStore* store) {
    if (job.module == nullptr ||
        job.method_id >= job.module->method_table.size()) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "J4: unknown method id " +
                                 std::to_string(job.method_id));
    }
    const ugb::UGBModule& module = *job.module;

    // ---- capture: persistent IR reuse or fresh build (spec 12.1) --------
    version_hash_ =
        PersistentIrStore::method_version_hash(module, job.method_id);
    if (store != nullptr) {
        const uint64_t key =
            PersistentIrStore::store_key(module, job.method_id);
        PersistentIrEntry* entry = store->find_reusable(key, version_hash_);
        if (entry != nullptr) {
            built_ = std::move(entry->built);
            stats_.reused_ir = true;
        }
    }
    if (built_ == nullptr) {
        j2::GraphBuilderParams bp;
        bp.module = &module;
        bp.method = &module.method_table[job.method_id];
        bp.profiles = job.profiles;
        bp.ics = job.ics;
        bp.klass_addrs = job.klass_addrs;
        bp.interop = job.interop;
        // No build budget (Rule 15): memory exhaustion is the graceful
        // degradation path, surfaced by the builder as a named refusal.
        bp.node_cap = SIZE_MAX;
        j2::BuildResult br = j2::build_graph(bp);
        if (!br.ok || br.out == nullptr) {
            const support::ErrorCode code =
                br.error == j2::BuildError::UnsupportedOpcode ||
                        br.error == j2::BuildError::BudgetExceeded
                    ? support::ErrorCode::Unimplemented
                    : support::ErrorCode::DecodeError;
            return support::fail(code, std::string("J4: build refused: ") +
                                           j2::build_error_message(br.error) +
                                           " at pc " +
                                           std::to_string(br.error_pc));
        }
        built_ = std::move(br.out);
    }

    budget_ = j4_budget(job);
    // Run 1 brings the graph onto the pipeline's deterministic trajectory
    // (for a reused graph it usually confirms stability immediately).
    summary_ = j3::run_j3_pipeline(*built_->graph, *built_, budget_,
                                   pipeline_stats_);
    ++stats_.pipeline_runs;
    fp_ = fingerprint(*built_->graph);
    fp_.iterations = 0;
    seen_.insert(fp_.graph_hash, 1);
    started_ = true;
    return {};
}

/// CEP:WHAT: Session::advance - exactly one pipeline iteration plus the termination evaluation.
/// CEP:WHY: The fixed-point loop (spec 12.2) must be ONE implementation shared by the synchronous engine and the stepwise worker; this is that unit.
/// CEP:STATUS: complete
/// CEP:FAILURE: none - the two stop edges (stability, repeated hash) are normal returns; a call before capture is a named programming error returned as Ran.
/// CEP:ASSUMES: run_j3_pipeline is deterministic and monotone-or-neutral per stage, so the fingerprint sequence converges or cycles in finitely many steps.
/// CEP:COST: @cold. One pipeline run + one fingerprint walk + one flat-map probe per iteration; real bodies need exactly one confirming advance.
/// CEP:EVIDENCE: j4_fixed_point_termination_invariants (pipeline_runs >= 2, stable edge); j4_worker_stepwise_j3_keeps_running (same loop via the worker).
Advance Session::advance() {
    if (!started_ || done_) {
        return done_ ? last_advance_ : Advance::Ran;
    }
    const uint64_t prev_hash = fp_.graph_hash;
    summary_ = j3::run_j3_pipeline(*built_->graph, *built_, budget_,
                                   pipeline_stats_);
    ++stats_.pipeline_runs;
    fp_ = fingerprint(*built_->graph);
    fp_.iterations = stats_.pipeline_runs - 1;
    if (fp_.graph_hash == prev_hash) {
        fp_.stable = true;  // fixed point reached (spec 12.2 stop edge 1)
        done_ = true;
        last_advance_ = Advance::Stable;
    } else if (seen_.contains(fp_.graph_hash)) {
        fp_.cycle = true;  // deterministic stop edge 2: repeated hash
        done_ = true;
        last_advance_ = Advance::Cycle;
    } else {
        seen_.insert(fp_.graph_hash, 1);
        last_advance_ = Advance::Ran;
    }
    stats_.fixed_point = fp_;
    return last_advance_;
}

/// CEP:WHAT: Session::emit - backend emission under the J4 tier identity with the summary identity binding.
/// CEP:WHY: Rule 19 (one backend for J2/J3/J4) and the xlea Identity law: the escape summary must name the exact graph the fixed point terminated on.
/// CEP:STATUS: complete
/// CEP:FAILURE: Backend refusals ride through as named Result failures.
/// CEP:ASSUMES: called only after the termination rule fired (done_).
/// CEP:COST: @cold. One backend emit (dominance placement + regalloc + instruction selection) - the same cost as a J3 emission.
/// CEP:EVIDENCE: j4_fixed_point_termination_invariants (summary hash == fixed-point hash).
support::Result<J2Code> Session::emit(const J2Job& job) {
    j2::PipelineStats core = pipeline_stats_.j2_core;
    core.scalar_replaced = pipeline_stats_.scalar_replaced;  // Rule 120
    support::Result<J2Code> out_code =
        j2::emit_optimized(job, *built_, core, Tier::J4);
    if (out_code) {
        out_code->summary = summary_;
        // The summary binds to the EXACT graph it was computed on (docs/
        // xlea.md section 4.1): the same canonical hash the fixed point
        // terminated on.
        out_code->summary.graph_hash = fp_.graph_hash;
        out_code->has_summary = true;
    }
    return out_code;
}

/// CEP:WHAT: Session::store_result - hand the pipeline-processed state back to the persistent store.
/// CEP:WHY: The next recompile must start from THIS state (spec 12.1); without it the persistent-IR property would be write-only. The state records where in the publication lifecycle the hand-off happened.
/// CEP:STATUS: complete
/// CEP:FAILURE: none - a null store or a missing built graph is a silent no-op by contract.
/// CEP:ASSUMES: the caller moved the emitted code out already; the graph state is consumable afterwards.
/// CEP:COST: @cold. One store put (linear match + one move) per publish/compile.
/// CEP:EVIDENCE: j4_persistent_ir_reuse_is_bit_identical (store.size() == 1 after compile).
void Session::store_result(PersistentIrStore* store, const J2Job& job,
                           PersistentIrEntry::State state) {
    if (store == nullptr || built_ == nullptr) return;
    const uint64_t key =
        PersistentIrStore::store_key(*job.module, job.method_id);
    PersistentIrEntry entry;
    entry.bytecode_version_hash = version_hash_;
    entry.built = std::move(built_);
    entry.summary = summary_;
    entry.fingerprint = fp_;
    entry.state = state;
    store->put(key, std::move(entry));
}

}  // namespace detail

/// CEP:WHAT: compile_j4 - the synchronous driver over the shared Session loop.
/// CEP:WHY: The J4 tier entry point; drives capture -> the fixed point -> emit -> store with no duplicated engine logic (the worker drives the same Session).
/// CEP:STATUS: complete
/// CEP:FAILURE: Named failures ride through from capture/emit; the method stays on its current tier (Rules 11/76).
/// CEP:ASSUMES: same as Session::capture (verified module, optional store).
/// CEP:COST: @cold. Sum of the session phases: build/reuse + pipeline_runs x pipeline + emit + store.
/// CEP:EVIDENCE: every test in tests/test_j4.cpp; the bench (tools/bench) measures the real cost.
support::Result<J2Code> compile_j4(const J2Job& job,
                                   PersistentIrStore* store,
                                   J4Stats* stats) {
    detail::Session session;
    auto captured = session.capture(job, store);
    if (!captured) return std::unexpected(captured.error());

    // The fixed-point loop (spec 12.2): stability or a repeated hash —
    // never an iteration cap.
    while (!session.done()) {
        const detail::Advance a = session.advance();
        (void)a;
    }

    support::Result<J2Code> out_code = session.emit(job);
    if (out_code && store != nullptr) {
        session.store_result(store, job);
    }
    if (stats != nullptr) *stats = session.stats();
    return out_code;
}

}  // namespace vortex::j4
