// J3 — adaptive full optimizing JIT (docs/tier-j3.md): full SoN + CIOG,
// budgeted aggressive pipeline (60 passes), full RBPD.
#pragma once

#include <cstdint>

#include "vortex/deopt/rbpd.hpp"
#include "vortex/ir/ciog.hpp"
#include "vortex/ir/son_graph.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::j3 {

/// J3 compile budgets (docs/tier-j3.md section 1). Reaching a limit still
/// emits good code.
struct Budget {
    size_t graph_node_budget = 200'000;
    uint32_t inline_budget = 128;
    uint32_t polyvariant_context_budget = 8;
    uint32_t loop_unroll_budget = 8;
    bool vectorization_enabled = true;
    uint64_t wall_time_budget_ms = 50;
    size_t code_size_budget = 1 << 20;
};

/// M0: contract defined; the 60-pass pipeline lands in M3 in three waves
/// (scalar core -> inlining/CIOG -> loops/vector/backend).
class FullJit {
public:
    explicit FullJit(Budget budget = {}) noexcept : budget_(budget) {}

    support::Result<uint32_t> compile(const ugb::UGBModule& module,
                                      uint32_t method_id);

    const Budget& budget() const noexcept { return budget_; }

private:
    Budget budget_;
};

}  // namespace vortex::j3
