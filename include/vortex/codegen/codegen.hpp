// Abstract code generation contract. Every tier (J1 stencils, J2/J3/J4 code
// emission) lowers to a target through this interface, keeping the tiers
// target-agnostic (docs/roadmap.md: ARM64 lands behind the same contract).
#pragma once

#include <cstdint>
#include <vector>

#include "vortex/support/result.hpp"

namespace vortex::codegen {

enum class TargetISA : uint8_t { X86_64, ARM64 };

struct Relocation {
    enum class Kind : uint8_t { Rel32Branch, Rel32Call, Abs64 };
    Kind kind = Kind::Rel32Branch;
    size_t offset = 0;       // patch position within the code buffer
    uint32_t symbol_id = 0;  // opaque target id resolved by the owner
};

/// A code buffer with relocation tracking. Tiers append bytes; the publisher
/// resolves relocations and hands the buffer to the W^X code memory manager
/// (infra/security).
class CodeBuffer {
public:
    void emit8(uint8_t b) { code_.push_back(b); }
    void emit16(uint16_t v) {
        code_.push_back(static_cast<uint8_t>(v & 0xFF));
        code_.push_back(static_cast<uint8_t>(v >> 8));
    }
    void emit32(uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            code_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
        }
    }
    void emit64(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            code_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
        }
    }
    void emit_bytes(const uint8_t* p, size_t n) {
        code_.insert(code_.end(), p, p + n);
    }
    void add_relocation(Relocation::Kind kind, size_t offset, uint32_t symbol) {
        relocations_.push_back(Relocation{kind, offset, symbol});
    }

    std::vector<uint8_t>& code() noexcept { return code_; }
    const std::vector<uint8_t>& code() const noexcept { return code_; }
    std::vector<Relocation>& relocations() noexcept { return relocations_; }
    size_t size() const noexcept { return code_.size(); }

private:
    std::vector<uint8_t> code_;
    std::vector<Relocation> relocations_;
};

/// Capabilities a code generator exposes to the tiers (docs/tier-j3.md pass 56).
class CodeGen {
public:
    virtual ~CodeGen() = default;
    virtual TargetISA isa() const noexcept = 0;
    virtual CodeBuffer& buffer() noexcept = 0;
};

}  // namespace vortex::codegen
