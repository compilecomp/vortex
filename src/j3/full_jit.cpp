// J3 driver (docs/tier-j3.md; docs/roadmap.md M3): build -> full 60-stage
// pipeline -> shared backend -> publication with the escape summary. The
// run path reuses run_j2's contract (J1 ABI, captured deopt, RBPD region
// accounting) under the J3 tier identity.
//
// Determinism (Rule 56): the pipeline is a fixed stage order; the graph
// hash is an FNV-1 walk over live nodes in id order (kind, aux, payload,
// inputs) — stable across runs of the same input, which is what the
// escape-summary Identity law binds to (docs/xlea.md section 4.1).
#include "vortex/j3/full_jit.hpp"

namespace vortex::j3 {

namespace {

uint64_t graph_hash_of(const ir::Graph& g) {
    uint64_t h = 1469598103934665603ull;  // FNV offset basis
    auto mix = [&h](uint64_t b) {
        h ^= b;
        h *= 1099511628211ull;  // FNV prime
    };
    for (const ir::Node& n : g.nodes()) {
        if (n.dead) continue;
        mix(static_cast<uint64_t>(n.kind));
        mix(n.aux);
        mix(static_cast<uint64_t>(n.const_value));
        mix(n.data_inputs.size());
        for (const ir::NodeId in : n.data_inputs) mix(in);
    }
    return h;
}

}  // namespace

support::Result<J2Code> compile_j3(const J2Job& job) {
    if (job.module == nullptr ||
        job.method_id >= job.module->method_table.size()) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "J3: unknown method id " +
                                 std::to_string(job.method_id));
    }
    const ugb::UGBMethod& method = job.module->method_table[job.method_id];

    j2::GraphBuilderParams bp;
    bp.module = job.module;
    bp.method = &method;
    bp.profiles = job.profiles;
    bp.ics = job.ics;
    bp.klass_addrs = job.klass_addrs;
    bp.interop = job.interop;
    bp.node_cap = job.node_cap;
    j2::BuildResult br = j2::build_graph(bp);
    if (!br.ok || br.out == nullptr) {
        const support::ErrorCode code =
            br.error == j2::BuildError::UnsupportedOpcode ||
                    br.error == j2::BuildError::BudgetExceeded
                ? support::ErrorCode::Unimplemented
                : support::ErrorCode::DecodeError;
        return support::fail(code, std::string("J3: build refused: ") +
                                       j2::build_error_message(br.error) +
                                       " at pc " +
                                       std::to_string(br.error_pc));
    }
    j2::BuiltGraph& built = *br.out;

    J3Budget budget;
    budget.node_cap = job.node_cap;
    // Rule 59/131: bit i of pass_kill_switches disables stage i+1. The J2
    // core sub-pipeline honors its OWN bits (PassControl numbering); the
    // J3-only stages honor the stage-index bits.
    budget.pass_control = ~job.pass_kill_switches;
    J3Stats stats;
    const ir::EscapeSummary summary =
        run_j3_pipeline(*built.graph, built, budget, stats);

    j2::PipelineStats core = stats.j2_core;
    core.scalar_replaced = stats.scalar_replaced;  // Rule 120: J3 telemetry
    // The graph hash binds the escape summary to the exact compiled graph
    // (docs/xlea.md section 4.1 Identity law).
    support::Result<j2::J2Code> out_code =
        j2::emit_optimized(job, built, core, Tier::J3);
    if (out_code) {
        out_code->summary = summary;
        out_code->summary.graph_hash = graph_hash_of(*built.graph);
        out_code->has_summary = true;
    }
    return out_code;
}

support::Result<J3Executable> publish_j3(const J2Code& code,
                                         const ir::EscapeSummary& summary,
                                         infra::CodeRange* range) {
    auto core = j2::publish_j2(code, range);
    if (!core) return std::unexpected(core.error());
    J3Executable ex;
    ex.core = std::move(*core);
    ex.summary = summary;
    return ex;
}

support::Result<TaggedValue> run_j3(J3Executable& ex,
                                    j1::J1Bindings& bindings,
                                    std::span<const TaggedValue> args,
                                    vm::Interpreter& interp) {
    // Identical execution + deopt contract to J2 (one ABI, one runtime);
    // the tier identity differs only in the region table's tier field and
    // the summary publication above.
    return j2::run_j2(ex.core, bindings, args, interp);
}

}  // namespace vortex::j3
