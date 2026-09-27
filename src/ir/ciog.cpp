// CIOG — Call/Inline/Outline Graph overlay (docs/ir-son-ciog.md section 2).
// M3 implements the construction + maintenance contracts: one CallNode per
// call site, one InlineSite per spliced callee (keyed by the spliced body's
// FrameState — the deopt continuation chain), one OutlineRegion per deopt
// region. All records are id-keyed (Rule 5); construction is deterministic
// (the caller walks nodes in id order).
#include "vortex/ir/ciog.hpp"

namespace vortex::ir {

support::Result<uint32_t> CiogOverlay::record_call(uint32_t call_site_id,
                                          uint32_t bytecode_pc) {
    for (const CallNode& c : calls_) {
        if (c.call_site_id == call_site_id) return c.call_site_id;
    }
    CallNode c;
    c.call_site_id = call_site_id;
    c.bytecode_pc = bytecode_pc;
    c.decision = InlineDecision::Defer;
    calls_.push_back(c);
    return static_cast<uint32_t>(calls_.size() - 1);
}

support::Result<uint32_t> CiogOverlay::build_inline_site(uint32_t call_node,
                                                uint32_t deopt_continuation) {
    for (const InlineSite& s : inline_sites_) {
        if (s.call_node == call_node) return s.call_node;
    }
    InlineSite s;
    s.call_node = call_node;
    s.deopt_continuation = deopt_continuation;
    s.context_key = next_context_key_++;
    inline_sites_.push_back(s);
    return static_cast<uint32_t>(inline_sites_.size() - 1);
}

support::Result<uint32_t> CiogOverlay::extract_outline(uint32_t entry_node,
                                              OutlineKind kind) {
    OutlineRegion r;
    r.kind = kind;
    r.entry_node = entry_node;
    r.region_id = next_region_id_++;
    outlines_.push_back(r);
    return r.region_id;
}

}  // namespace vortex::ir
