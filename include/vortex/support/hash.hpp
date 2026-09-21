// Small hash helpers shared by value numbering, graph fingerprints and the
// persistent IR identity checks (docs/tier-j4.md section 7).
#pragma once

#include <cstdint>
#include <span>

namespace vortex::support {

constexpr uint64_t kFnv1aBasis = 0xcbf29ce484222325ull;
constexpr uint64_t kFnv1aPrime = 0x100000001b3ull;

constexpr uint64_t fnv1a_mix(uint64_t hash, uint64_t value) noexcept {
    hash ^= value;
    hash *= kFnv1aPrime;
    return hash;
}

constexpr uint64_t fnv1a_span(std::span<const uint8_t> bytes) noexcept {
    uint64_t hash = kFnv1aBasis;
    for (uint8_t b : bytes) {
        hash ^= b;
        hash *= kFnv1aPrime;
    }
    return hash;
}

}  // namespace vortex::support
