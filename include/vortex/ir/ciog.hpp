// CIOG — Call/Inline/Outline Graph overlay (docs/ir-son-ciog.md section 2).
// M0 fixes the contracts for 3 of the 8 CIOG node types (CallNode, InlineSite,
// OutlineRegion); ContextKey, SpecializationKey, DeoptRegion, ColdRegion and
// BarrierRegion join with the J3 pipeline (docs/roadmap.md, M3).
#pragma once

#include <cstdint>
#include <vector>

#include "vortex/ir/son_graph.hpp"

namespace vortex::ir {

enum class InlineDecision : uint8_t {
    Inline,
    DontInline,
    Defer,
    SpeculativeInline,
    PolyvariantInline,
};

enum class OutlineKind : uint8_t {
    OutlineDeopt,
    OutlineCold,
    OutlineBarrier,
    OutlineSlowPath,
    OutlineInlineFailure,
    OutlineException,
    OutlineUncommonTrap,
};

/// CallNode — call site with profile-driven inline decision
/// (docs/ir-son-ciog.md 2.1).
struct CallNode {
    uint32_t call_site_id = 0;
    uint32_t bytecode_pc = 0;
    uint64_t execution_count = 0;
    uint64_t failure_count = 0;
    InlineDecision decision = InlineDecision::Defer;
    std::vector<uint32_t> candidate_callees;
};

/// InlineSite — connects a caller to an inlined callee (2.2).
struct InlineSite {
    uint32_t call_node = 0;
    uint32_t callee_method = 0;
    uint32_t context_key = 0;
    uint32_t deopt_continuation = 0;
};

/// OutlineRegion — every outline becomes a partial deopt region (2.3 + RBPD).
struct OutlineRegion {
    OutlineKind kind = OutlineKind::OutlineCold;
    uint32_t region_id = 0;  // deopt::RegionTable id
    uint32_t entry_node = 0;
    uint32_t exit_node = 0;
};

class CiogOverlay {
public:
    explicit CiogOverlay(Graph& graph) noexcept : graph_(graph) {}

    // Contract stubs (docs/roadmap.md, M3): construction + maintenance.
    support::Result<uint32_t> record_call(uint32_t call_site_id,
                                          uint32_t bytecode_pc);
    support::Result<uint32_t> build_inline_site(uint32_t call_node,
                                                uint32_t callee_method);
    support::Result<uint32_t> extract_outline(uint32_t entry_node,
                                              OutlineKind kind);

    std::span<const CallNode> calls() const noexcept { return calls_; }

private:
    Graph& graph_;
    std::vector<CallNode> calls_;
    std::vector<InlineSite> inline_sites_;
    std::vector<OutlineRegion> outlines_;
};

}  // namespace vortex::ir
