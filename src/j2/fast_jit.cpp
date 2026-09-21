#include "vortex/j2/fast_jit.hpp"

namespace vortex::j2 {

support::Result<FastJitResult> FastJit::compile(const ugb::UGBModule& module,
                                                uint32_t method_id) {
    (void)module;
    (void)method_id;
    // Light SoN pipeline lands in M2 (docs/roadmap.md) on the ir::Graph
    // substrate shipped in M0.
    return support::unimplemented("J2 light Sea-of-Nodes pipeline (roadmap M2)");
}

}  // namespace vortex::j2
