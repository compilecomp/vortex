#include "vortex/infra/cpu_dispatch.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#endif

#include <cstring>

namespace vortex::infra {

CpuInfo detect_cpu() {
    CpuInfo info;
#if defined(__x86_64__) || defined(_M_X64)
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0, &eax, &ebx, &ecx, &edx)) {
        char vendor[13] = {};
        std::memcpy(vendor + 0, &ebx, 4);
        std::memcpy(vendor + 8, &edx, 4);
        std::memcpy(vendor + 4, &ecx, 4);
        info.vendor = vendor;
    }
    // Leaf 1 ECX: bit 12 FMA, bit 20 SSE4.2, bit 23 POPCNT, bit 25 AES-NI,
    // bit 28 AVX (docs/infrastructure/07: runtime CPU feature detection).
    if (__get_cpuid_count(1, 0, &eax, &ebx, &ecx, &edx)) {
        if (ecx & (1u << 12)) info.features |= 1ull << 8;  // FMA
        if (ecx & (1u << 20)) info.features |= 1ull << 0;  // SSE4.2
        if (ecx & (1u << 23)) info.features |= 1ull << 1;  // POPCNT
        if (ecx & (1u << 25)) info.features |= 1ull << 2;  // AES-NI
        if (ecx & (1u << 28)) info.features |= 1ull << 5;  // AVX
    }
    // Leaf 7 EBX: bit 3 BMI1, bit 5 AVX2, bit 8 BMI2, bit 16 AVX-512F,
    // bit 29 SHA-NI.
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if (ebx & (1u << 3)) info.features |= 1ull << 4;   // BMI1 (BMI2 class)
        if (ebx & (1u << 5)) info.features |= 1ull << 6;   // AVX2
        if (ebx & (1u << 8)) info.features |= 1ull << 4;   // BMI2
        if (ebx & (1u << 16)) info.features |= 1ull << 7;  // AVX-512F
        if (ebx & (1u << 29)) info.features |= 1ull << 3;  // SHA-NI
    }
#endif
    return info;
}

FeatureLevel feature_level(FeatureSet f) {
    if (f & (1ull << 7)) return FeatureLevel::Avx512;  // AVX-512F
    if (f & (1ull << 6)) return FeatureLevel::Avx2;    // AVX2
    if (f & (1ull << 5)) return FeatureLevel::Avx;     // AVX
    if (f & (1ull << 0)) return FeatureLevel::Sse4;    // SSE4.2
    return FeatureLevel::Baseline;
}

const char* feature_level_name(FeatureLevel level) noexcept {
    switch (level) {
    case FeatureLevel::Baseline: return "baseline";
    case FeatureLevel::Sse4: return "sse4.2";
    case FeatureLevel::Avx: return "avx";
    case FeatureLevel::Avx2: return "avx2";
    case FeatureLevel::Avx512: return "avx512";
    }
    return "?";
}

}  // namespace vortex::infra
