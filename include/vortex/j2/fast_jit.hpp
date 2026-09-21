// J2 — fast optimizing JIT (docs/tier-j2.md): light SoN pipeline, hard budgets,
// graceful degradation.
#pragma once

#include <cstdint>

#include "vortex/ir/son_graph.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::j2 {

/// J2 compile budget (docs/tier-j2.md section 1). Exceeding a budget must
/// degrade gracefully — never fail the compile.
struct Budget {
    size_t node_cap = 20'000;
    uint32_t inline_depth_cap = 2;
    uint32_t inline_site_cap = 8;
    uint32_t loop_pass_cap = 4;
    bool vectorization_enabled = false;
    bool exhaustive_fixed_point = false;
};

struct FastJitResult {
    uint32_t method_id = 0;
    bool budget_exceeded = false;  // true => output emitted mid-pipeline
};

/// M0: contract defined; pipeline lands in M2 (docs/roadmap.md).
class FastJit {
public:
    explicit FastJit(Budget budget = {}) noexcept : budget_(budget) {}

    support::Result<FastJitResult> compile(const ugb::UGBModule& module,
                                           uint32_t method_id);

    const Budget& budget() const noexcept { return budget_; }

private:
    Budget budget_;
};

}  // namespace vortex::j2
