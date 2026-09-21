// J1 — no-IR stencil baseline JIT (docs/tier-j1.md).
#pragma once

#include <cstdint>

#include "vortex/codegen/x64/assembler.hpp"
#include "vortex/infra/security.hpp"
#include "vortex/j1/stencil.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::j1 {

/// Compilation task: one method, from the bottom of the tier stack.
struct BaselineJob {
    const ugb::UGBModule* module = nullptr;
    uint32_t method_id = 0;
};

/// Compiled baseline artifact: code + compact metadata
/// (docs/tier-j1.md sections 8-9).
struct BaselineCode {
    uint32_t method_id = 0;
    std::vector<uint8_t> code;       // ready for W^X publication
    std::vector<uint32_t> osr_entries;   // bytecode PCs with OSR stubs
    std::vector<uint8_t> gc_maps;    // compact root maps
    std::vector<uint8_t> deopt_records;  // BaselineDeoptRecord stream
};

/// The baseline compiler. M0: contract defined, corpus in M1
/// (docs/roadmap.md M1 DoD: J1 output matches T0 observable behavior).
class BaselineJit {
public:
    explicit BaselineJit(StencilTable& stencils) noexcept : stencils_(stencils) {}

    support::Result<BaselineCode> compile(const BaselineJob& job);

    const StencilTable& stencils() const noexcept { return stencils_; }

private:
    StencilTable& stencils_;
};

}  // namespace vortex::j1
