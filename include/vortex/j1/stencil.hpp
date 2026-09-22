// J1 stencil model (docs/tier-j1.md sections 2-3).
#pragma once

#include <cstdint>
#include <vector>

namespace vortex::j1 {

/// Patch site types (docs/tier-j1.md section 2).
enum class PatchKind : uint8_t {
    VirtualRegister,
    ConstantIndex,
    IcSlot,
    BranchTarget,
    CallTarget,
    CardTableBase,
    ThreadLocalSlot,
    ProfileCounter,
    BytecodePc,
    DeoptHandle,
};

struct PatchSite {
    PatchKind kind = PatchKind::VirtualRegister;
    uint32_t offset = 0;   // byte offset inside template_bytes
    uint8_t operand = 0;   // patch-specific operand selector
};

/// A precompiled machine-code template for one bytecode or bytecode pattern.
struct Stencil {
    uint16_t opcode = 0;          // UGB opcode this stencil implements
    bool speculative = false;     // typed form with fallback path
    std::vector<uint8_t> bytes;   // template machine code
    std::vector<PatchSite> patch_sites;
    // Template-internal offset of the IC-guard slow body (always-slow jmp
    // target). 0 = no slow path (constant/control-flow templates).
    uint32_t slow_path_offset = 0;
    uint32_t gc_map_offset = 0;
    uint32_t deopt_record_offset = 0;
};

/// A fused stencil for a bytecode sequence (docs/tier-j1.md section 3).
struct Superstencil {
    std::vector<uint16_t> sequence;  // UGB opcode sequence matched exactly
    std::vector<uint8_t> bytes;
    std::vector<PatchSite> patch_sites;
};

/// The stencil corpus. M0 ships the model + selection contract; the x86-64
/// corpus is generated in M1 (docs/roadmap.md).
class StencilTable {
public:
    void register_stencil(Stencil s);
    void register_superstencil(Superstencil s);

    /// Selects the stencil for one opcode (typed variant honored when the
    /// profile is stable; generic fallback otherwise).
    const Stencil* select(uint16_t opcode, bool profile_stable) const;

    /// Matches the longest superstencil at `pc` over `code`; returns nullptr
    /// when no fusion applies.
    const Superstencil* match_superstencil(const uint16_t* code,
                                           size_t max_ops) const;

    size_t stencil_count() const noexcept { return stencils_.size(); }
    size_t superstencil_count() const noexcept { return superstencils_.size(); }

private:
    std::vector<Stencil> stencils_;
    std::vector<Superstencil> superstencils_;
};

}  // namespace vortex::j1
