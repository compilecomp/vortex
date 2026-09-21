// Infra 7 — Hardware Intrinsics & CPU Dispatch
// (docs/infrastructure/07-cpu-dispatch.md).
//
// M0 implements x86-64 CPUID feature detection. Multi-versioning stubs and
// hardware-extension lowering are contract-stubbed.
#pragma once

#include <cstdint>
#include <string>

namespace vortex::infra {

enum class Feature : uint64_t {
    Sse42 = 1ull << 0,
    Popcnt = 1ull << 1,
    AesNi = 1ull << 2,
    ShaNi = 1ull << 3,
    Bmi2 = 1ull << 4,
    Avx = 1ull << 5,
    Avx2 = 1ull << 6,
    Avx512F = 1ull << 7,
    Fma = 1ull << 8,
};

using FeatureSet = uint64_t;

struct CpuInfo {
    FeatureSet features = 0;
    std::string vendor;
    uint32_t cores = 1;
};

/// Detects CPU features at runtime. x86-64 uses CPUID; unknown hosts report an
/// empty feature set and the portable path is used.
CpuInfo detect_cpu();

/// Monotone feature levels consumed by the vectorizer and multi-versioner
/// (docs/infrastructure/07 section: multi-versioning / fat binaries in RAM).
enum class FeatureLevel : uint8_t { Baseline, Sse4, Avx, Avx2, Avx512 };

FeatureLevel feature_level(FeatureSet features);

const char* feature_level_name(FeatureLevel level) noexcept;

}  // namespace vortex::infra
