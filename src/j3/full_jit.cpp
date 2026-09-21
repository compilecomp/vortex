#include "vortex/j3/full_jit.hpp"

namespace vortex::j3 {

support::Result<uint32_t> FullJit::compile(const ugb::UGBModule& module,
                                           uint32_t method_id) {
    (void)module;
    (void)method_id;
    // The budgeted 60-pass pipeline lands in M3 (docs/roadmap.md) in three
    // waves on top of ir::Graph + ir::CiogOverlay + deopt::RegionTable.
    return support::unimplemented("J3 full pipeline (roadmap M3)");
}

}  // namespace vortex::j3
