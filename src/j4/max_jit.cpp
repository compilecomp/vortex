#include "vortex/j4/max_jit.hpp"

#include "vortex/support/hash.hpp"

namespace vortex::j4 {

support::Result<uint32_t> MaxJit::compile(const ugb::UGBModule& module,
                                          uint32_t method_id) {
    (void)module;
    (void)method_id;
    // The deterministic fixed-point engine lands in M4 (docs/roadmap.md).
    // DoD there: bit-identical output across repeated compiles; no search.
    return support::unimplemented("J4 deterministic engine (roadmap M4)");
}

FixedPointState MaxJit::fingerprint(const ir::Graph& graph) {
    FixedPointState state;
    uint64_t hash = 0xcbf29ce484222325ull;  // FNV-1a basis
    for (const ir::Node* n : graph.nodes()) {
        if (n->dead) continue;
        hash ^= static_cast<uint64_t>(n->kind);
        hash *= 0x100000001b3ull;
        hash ^= n->input_count();
        hash *= 0x100000001b3ull;
        ++state.node_count;
    }
    state.graph_hash = hash;
    return state;
}

}  // namespace vortex::j4
