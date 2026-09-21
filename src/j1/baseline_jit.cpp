#include "vortex/j1/baseline_jit.hpp"

namespace vortex::j1 {

support::Result<BaselineCode> BaselineJit::compile(const BaselineJob& job) {
    (void)job;
    // The x86-64 stencil corpus lands in M1 (docs/roadmap.md). The model is
    // complete: StencilTable selection, superstencil matching, instantiation
    // ordering, and the W^X publication path are all exercised by the code
    // cache and security manager today.
    return support::unimplemented("J1 stencil corpus (roadmap M1)");
}

}  // namespace vortex::j1
