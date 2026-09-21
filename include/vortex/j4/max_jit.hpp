// J4 — max deterministic optimizing JIT (docs/tier-j4.md): no artificial
// budget, no search, deterministic fixed-point pipeline, persistent IR,
// background compilation, incremental region publication.
#pragma once

#include <cstdint>

#include "vortex/ir/son_graph.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::j4 {

/// The 15 final-tier rules (docs/tier-j4.md section 11) are normative for this
/// component. In particular:
///   - no time/node/inline/pass-count/code-size budget;
///   - deterministic pass ordering, rewrite rules and cost models only;
///   - fixed-point iteration with termination invariants (graph hash +
///     lattice state);
///   - profile-directed but never search-directed.
struct FixedPointState {
    uint64_t graph_hash = 0;
    size_t node_count = 0;
    uint32_t iteration = 0;
    bool stable = false;
};

/// M0: contract defined; the deterministic engine lands in M4. The DoD there
/// requires bit-identical output across repeated compiles of the same input.
class MaxJit {
public:
    support::Result<uint32_t> compile(const ugb::UGBModule& module,
                                      uint32_t method_id);

    /// Computes the fixed-point stability fingerprint used by the pipeline's
    /// termination rule (graph hash + node count + lattice state).
    static FixedPointState fingerprint(const ir::Graph& graph);
};

}  // namespace vortex::j4
