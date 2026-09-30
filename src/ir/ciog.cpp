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
    // Return contract (M4 clarification): the value is the CallNode's INDEX
    // in `calls_` — the SAME meaning on the fresh and the idempotent path.
    // (The old code returned the site id on re-application and the index on
    // first application — two meanings for one Result.) Re-application is
    // otherwise a no-op: the recorded bytecode_pc of the first application
    // wins, so a fixed-point re-run cannot rewrite metadata.
    for (uint32_t i = 0; i < calls_.size(); ++i) {
        if (calls_[i].call_site_id == call_site_id) return i;
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
    // Return contract (M4 clarification): the InlineSite's INDEX in
    // `inline_sites_` on both paths. Idempotent by construction: the first
    // application's context_key is kept — context keys are assigned in
    // discovery order and a re-run must not renumber them.
    for (uint32_t i = 0; i < inline_sites_.size(); ++i) {
        if (inline_sites_[i].call_node == call_node) return i;
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
    // Idempotent by (entry_node, kind) (M4 clarification): a fixed-point
    // re-run of the construction stage must not append duplicate outline
    // regions for the same entry — the first region_id wins.
    for (const OutlineRegion& r : outlines_) {
        if (r.entry_node == entry_node && r.kind == kind) return r.region_id;
    }
    OutlineRegion r;
    r.kind = kind;
    r.entry_node = entry_node;
    r.region_id = next_region_id_++;
    outlines_.push_back(r);
    return r.region_id;
}

}  // namespace vortex::ir
