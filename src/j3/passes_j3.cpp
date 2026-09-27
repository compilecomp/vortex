// J3 pass pipeline driver (docs/tier-j3.md section 2 — the 60 named stages
// in three waves). Every stage slot runs exactly once per compile: it
// either transforms the graph, delegates to the shared J2 scalar core /
// shared backend, or reports NotApplicable with a named domain reason
// (Rule 76 applied to telemetry — no fake passes, no silent no-ops).
//
// Transforming stages implemented for M3 (one pass per file, src/j3/):
//   pass_redundant_load_elim.cpp   stage 7  PSE / store-to-load forwarding
//   pass_range_analysis.cpp        stage 10 Smi interval lattice
//   pass_guard_dominance.cpp       stages 11/26/41/42 guard redundancy
//   pass_scalar_replacement.cpp    stages 12+16 EA + scalar replacement
//   pass_deferred_field_init.cpp   stage 19 deferred field init
//   pass_ciog.cpp                  stage 20 CIOG overlay construction
//   pass_escape_summary.cpp        stage 29 escape-summary publication
//   pass_licm.cpp                  stages 30+31 loop id + LICM
//   pass_bounds_check_elim.cpp     stage 36 range-driven BCE
//
// Domain-gated stages carry their reason in the stage table below; they
// light up when the guest ISA or the tier above provides their domain
// (docs/tier-j3.md is updated with the same mapping).
#include "vortex/j3/passes.hpp"
#include "passes_internal.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>

#include "../j2/passes_internal.hpp"
#include "vortex/ir/ciog.hpp"
#include "vortex/runtime/object_model.hpp"
#include "vortex/support/containers.hpp"

namespace vortex::j3 {

using namespace vortex::j2;
using ir::Graph;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;
using BuiltGraph = j2::BuiltGraph;

// ---- the 60-stage driver -------------------------------------------------------
//
// Slot map (docs/tier-j3.md section 2 numbering; "J2 core" = the delegated
// j2::run_pipeline stages; "backend" = shared emit_optimized stages). Every
// NotApplicable carries the named domain reason — the same mapping is
// documented in docs/tier-j3.md.
ir::EscapeSummary run_j3_pipeline(Graph& graph, BuiltGraph& built,
                                  const J3Budget& budget, J3Stats& stats) {
    auto& st = stats.stages;
    auto na = [](const char* why) {
        StageResult r;
        r.status = StageStatus::NotApplicable;
        r.reason = why;
        return r;
    };
    auto ran = [](uint32_t n) {
        StageResult r;
        r.status = StageStatus::Ran;
        r.transformations = n;
        return r;
    };
    auto delegated = [](const char* what) {
        StageResult r;
        r.status = StageStatus::Delegated;
        r.reason = what;
        return r;
    };
    for (auto& s : st) s.result.status = StageStatus::Killed;
    // Rule 59/131: a killed transforming stage is a NO-OP with a named
    // status (never a fake Ran). Slot i (0-based) = stage i+1; bit i of
    // budget.pass_control set = stage ENABLED (the driver runs with all
    // bits set; the caller passes ~kills).
    const auto enabled = [&](uint32_t slot) {
        return slot < 60 &&
               (budget.pass_control & (1ull << slot)) != 0;
    };
    const auto killed = [&](uint32_t slot) {
        StageResult r;
        r.status = StageStatus::Killed;
        r.reason = "kill switch";
        st[slot] = {st[slot].name, r};
    };

    // Wave 1 — scalar core (stages 1-19). Stages 2-5, 8, 9, 21-23, 47-49 run
    // inside the shared J2 pipeline (PassControl bits); stage 1 is the
    // shared builder.
    j2::PipelineBudget core;
    core.node_cap = budget.node_cap;
    core.inline_depth_cap = budget.inline_depth_cap;
    core.inline_site_cap = budget.inline_site_cap;
    core.inline_callee_node_cap = budget.inline_callee_node_cap;
    core.pass_control = ~0ull;
    stats.j2_core = j2::run_pipeline(graph, built, core);
    st[0] = {"GraphBuilding", delegated("j2 build_graph (run by driver)")};
    st[1] = {"Canonicalization", delegated("j2 core pipeline")};
    st[2] = {"SCCP", delegated("j2 core pipeline")};
    st[3] = {"GVN", delegated("j2 core pipeline")};
    st[4] = {"DCE", delegated("j2 core pipeline")};
    st[5] = {"PRE",
             na("edge-predication needs SSA reconstruction (J4); full "
                "redundancy covered by GVN, availability by stage 7")};
    if (enabled(6)) {
      st[6] = {"PSE", ran(stats.loads_forwarded =
                            redundant_load_elim(graph, built))};
    } else {
      killed(6);
    }
    st[7] = {"StrengthReduction", delegated("j2 core pipeline")};
    st[8] = {"AlgebraicSimplification", delegated("j2 core pipeline")};
    {
        std::vector<Range> ranges;
        ranges_of(graph, built, ranges);
        st[9] = {"RangeAnalysis", ran(0)};  // analysis feeding 36
        // stash for BCE via a second computation (deterministic recompute)
        if (enabled(35)) {
            st[35] = {"BoundsCheckElimination",
                      ran(stats.bounds_killed =
                              eliminate_bounds_checks(graph, built, ranges))};
        } else {
            killed(35);
        }
    }
    if (enabled(10)) {
      st[10] = {"NullCheckElimination",
                ran(stats.guards_dominated =
                        eliminate_dominated_guards(graph, built))};
    } else {
      killed(10);
    }
    // st[11] EA / st[15] scalar replacement run AFTER wave-2 inlining (the
    // merged-graph requirement); slotted below in program order. The EA
    // itself is the escape-set analysis inside scalar_replace (stage 16) —
    // reported there, not as a phantom standalone transform (Rule 76).
    st[11] = {"EscapeAnalysis",
              na("escape-set analysis runs inside stage 16 "
                 "(scalar_replace); reported there")};
    st[12] = {"PartialEscapeAnalysis",
              na("materialization-on-demand is J4; proven EA (12) covers "
                 "the M3 non-escaping domain")};
    st[13] = {"TemporalPEA", na("J4 (suspension-visible objects)")};
    st[14] = {"ConnectionAnalysis",
              na("full CI is J4; the escape-arc subset runs in stage 12")};
    if (enabled(15)) {
      st[15] = {"ScalarReplacement",
              ran(stats.scalar_replaced = scalar_replace(graph, built))};
    } else {
      killed(15);
    }
    st[16] = {"LockElision", na("no monitors in the M0 guest ISA")};
    st[17] = {"ObjectSlicing", na("no narrowing op in the M0 guest ISA")};
    if (enabled(18)) {
      st[18] = {"DeferredFieldInit",
              ran(deferred_field_init(graph, built))};
    } else {
      killed(18);
    }

    // Wave 2 — inlining / CIOG (stages 20-29). The J2 core pipeline already
    // ran adaptive+speculative inlining and IC devirtualization over the
    // graph (delegated); this wave records the overlay + publishes summaries.
    if (enabled(19)) {
        ir::CiogOverlay ciog(graph);
        st[19] = {"CIOGConstruction", ran(build_ciog(graph, built, ciog))};
    } else {
        killed(19);
    }
    st[20] = {"AdaptiveInlining", delegated("j2 core pipeline")};
    st[21] = {"SpeculativeInlining", delegated("j2 core pipeline")};
    st[22] = {"SpeculativeDevirtualization", delegated("j2 IC stage")};
    st[23] = {"StaticDevirtualization",
              na("needs the virtual-dispatch model in profiles (J4)")};
    st[24] = {"CrossFunctionVirtualization",
              na("whole-program devirt is J4/interop providers")};
    st[25] = {"ReceiverTypePropagation",
              delegated("stage 11 klass-guard propagation")};
    st[26] = {"PolyvariantSpecialization",
              na("context cloning is J4 (2-context split)")};
    st[27] = {"ContextSensitiveInlining",
              na("context keys recorded by CIOG; cloning is J4")};
    st[28] = {"InterproceduralEA",
              na("the body's escape summary is built at pipeline end "
                 "(stage 29 output below); publication is not a "
                 "transform")};
    // Re-run the scalar tail after inlining exposed new redundancy
    // (honor the stage kill bits; refresh the telemetry slots).
    if (enabled(6)) {
        stats.loads_forwarded += redundant_load_elim(graph, built);
        st[6] = {"PSE",
                 ran(stats.loads_forwarded)};
    }
    if (enabled(15)) {
        stats.scalar_replaced += scalar_replace(graph, built);
        st[15] = {"ScalarReplacement", ran(stats.scalar_replaced)};
    }

    // Wave 3 — loops / guards / backend (stages 30-60).
    // Loop identification is the analysis inside LICM: only report it as
    // Ran when LICM's kill bit actually lets it run (Rule 59/120: no fake
    // Ran next to a Killed consumer).
    if (enabled(30)) {
      st[29] = {"LoopIdentification", ran(0)};  // runs inside LICM
      st[30] = {"LICM", ran(stats.hoisted_loops = licm(graph, built))};
    } else {
      st[29] = {"LoopIdentification",
                na("analysis runs inside LICM, which is killed")};
      killed(30);
    }
    st[31] = {"LoopPeeling", na("loop rewrite machinery is J4")};
    st[32] = {"LoopUnrolling", na("loop rewrite machinery is J4")};
    st[33] = {"LoopFusionFission", na("no array-loop pattern domain yet")};
    st[34] = {"LoopInterchange", na("no array-loop pattern domain yet")};
    // Wave 1 already ran (or killed) BCE under its own bit; this slot only
    // REFRESHES the count when the stage actually ran — a killed stage must
    // keep its Killed record, never a Ran(0) (Rules 59/76/120).
    if (enabled(35)) {
        st[35] = {"BoundsCheckElimination",
                  ran(stats.bounds_killed)};  // count from wave-1 slot
    }
    st[36] = {"InductionVariableOpt", na("loop-closed SSA update is J4")};
    st[37] = {"AutoVectorization", na("no SIMD opcodes in the M0 guest ISA")};
    st[38] = {"SLP", na("no SIMD opcodes in the M0 guest ISA")};
    st[39] = {"LoopAwareSLP", na("no SIMD opcodes in the M0 guest ISA")};
    if (enabled(40)) {
        st[40] = {"GuardRedundancy",
                  ran(stats.guards_dominated +=
                      eliminate_dominated_guards(graph, built))};
    } else {
        killed(40);
    }
    st[41] = {"GuardSubsumption", delegated("stage 41 dominance arm")};
    st[42] = {"GuardHoisting",
              na("guard motion needs FrameState re-derivation (J4)")};
    st[43] = {"GuardSinking", na("guard motion needs state re-derivation")};
    st[44] = {"GuardClustering", na("clustering is a layout pass (J4)")};
    st[45] = {"GuardStrengthReduction", delegated("j2 core pipeline")};
    st[46] = {"GuardFusion", delegated("j2 plan_emission fusion")};
    st[47] = {"EffectAwareScheduling", delegated("j2 plan_emission")};
    st[48] = {"GlobalCodeMotion", delegated("j2 dominance_placement")};
    st[49] = {"BarrierPlacement", delegated("backend emit_store policy")};
    st[50] = {"BarrierHoisting", na("single-store sites; nothing to hoist")};
    st[51] = {"BarrierElimination",
              na("own spec queued next (docs/xlea.md section 7 RBE)")};
    st[52] = {"OutlineExtraction",
              delegated("backend OOL stubs + region table")};
    st[53] = {"RegionFormation",
              delegated("emit_optimized build_region_table")};
    st[54] = {"RegisterAllocation", delegated("shared backend")};
    st[55] = {"InstructionSelection", delegated("shared backend")};
    st[56] = {"InstructionScheduling", delegated("shared backend")};
    st[57] = {"Peephole", delegated("shared backend")};
    st[58] = {"CodeLayout", delegated("shared backend")};
    st[59] = {"MetadataEmission", delegated("shared backend")};

    // Stage 29 output: the escape summary of the compiled body.
    const ir::EscapeSummary summary =
        summarize(graph, built, 0);  // graph_hash set by the driver caller
    stats.published_summaries = 1;
    return summary;
}

}  // namespace vortex::j3
