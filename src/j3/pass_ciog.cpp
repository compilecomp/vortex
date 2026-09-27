// Stage 20: CIOG construction (the ir/ciog.hpp overlay records).
// Split from src/j3/passes_j3.cpp — one pass per file.
#include "passes_internal.hpp"


namespace vortex::j3 {

using namespace vortex::j2;
using ir::Graph;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;
using BuiltGraph = j2::BuiltGraph;

// ---- stage 20: CIOG construction (ir/ciog.hpp overlay) ------------------------
uint32_t build_ciog(Graph& g, BuiltGraph& built, ir::CiogOverlay& ciog) {
    // CallNode per live call; InlineSite per spliced call site; OutlineRegion
    // per deopt region (guards + polls). Keyed by ids, never strings (R5).
    uint32_t recorded = 0;
    for (uint32_t id = 0; id < g.node_count(); ++id) {
        const Node& n = g.node(id);
        if (n.dead) continue;
        if (n.kind == NodeKind::Call) {
            // `ics` can be null (J3 on bodies compiled without an IC
            // table — the same condition specialize_ic guards); the pc
            // window is best-effort metadata, never a crash surface.
            const uint32_t pc =
                built.ics != nullptr &&
                        built.insn_of[id] < built.ics->size()
                    ? built.insn_of[id]
                    : 0;
            auto r = ciog.record_call(built.insn_of[id], pc);
            if (r) ++recorded;
        }
    }
    for (const auto& [fs_node, info] : built.inline_caller_frame) {
        auto r = ciog.build_inline_site(fs_node, info.frame_state);
        if (r) ++recorded;
    }
    for (const NodeId gid : built.guards) {
        if (g.node(gid).dead) continue;
        auto r = ciog.extract_outline(gid, ir::OutlineKind::OutlineDeopt);
        if (r) ++recorded;
    }
    return recorded;
}

}  // namespace vortex::j3
