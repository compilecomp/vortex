// Tier identifiers — shared by the tiering policy, the compile queue and the
// `vx` tool. T0 is the interpreter and is intentionally NOT counted as a JIT
// tier (docs/architecture.md invariant 2).
#pragma once

#include <cstdint>

namespace vortex {

enum class Tier : uint8_t {
    T0 = 0,  // speculative register interpreter
    J1 = 1,  // stencil baseline JIT (no IR)
    J2 = 2,  // fast optimizing JIT (light Sea-of-Nodes)
    J3 = 3,  // adaptive full optimizing JIT (full SoN + CIOG, budgeted)
    J4 = 4,  // max deterministic optimizing JIT (no budget, no search)
};

constexpr const char* tier_name(Tier t) noexcept {
    switch (t) {
    case Tier::T0: return "T0";
    case Tier::J1: return "J1";
    case Tier::J2: return "J2";
    case Tier::J3: return "J3";
    case Tier::J4: return "J4";
    }
    return "??";
}

}  // namespace vortex
