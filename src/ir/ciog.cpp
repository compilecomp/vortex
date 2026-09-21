#include "vortex/ir/ciog.hpp"

namespace vortex::ir {

support::Result<uint32_t> CiogOverlay::record_call(uint32_t call_site_id,
                                                   uint32_t bytecode_pc) {
    CallNode c;
    c.call_site_id = call_site_id;
    c.bytecode_pc = bytecode_pc;
    calls_.push_back(c);
    return static_cast<uint32_t>(calls_.size() - 1);
}

support::Result<uint32_t> CiogOverlay::build_inline_site(uint32_t call_node,
                                                         uint32_t callee_method) {
    if (call_node >= calls_.size()) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "build_inline_site: call node out of range");
    }
    inline_sites_.push_back(
        InlineSite{call_node, callee_method, 0, 0});
    return static_cast<uint32_t>(inline_sites_.size() - 1);
}

support::Result<uint32_t> CiogOverlay::extract_outline(uint32_t entry_node,
                                                       OutlineKind kind) {
    outlines_.push_back(OutlineRegion{kind, 0, entry_node, 0});
    return static_cast<uint32_t>(outlines_.size() - 1);
}

}  // namespace vortex::ir
