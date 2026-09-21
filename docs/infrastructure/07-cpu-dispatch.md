# Infra 7 — Hardware Intrinsics & CPU Dispatch System

J4 is deterministic, but hardware varies.

## Mechanisms

- **Runtime CPU feature detection.** Probe `CPUID`/`MRS` for AVX-512, AMX, SVE2,
  SME, etc.
- **Multi-versioning (fat binaries in RAM).** For hot vectorized loops, J4 emits
  multiple versions of the same region (one for SSE4, one for AVX2, one for
  AVX-512). The entry point contains a dynamic dispatch stub that patches itself
  to the best version on first execution.
- **Auto-vectorization intrinsics.** Expose hardware-specific instructions
  (AES-NI, SHA, polynomial multiply) as UGB `extension.hardware.*` capabilities,
  which J4 lowers directly to SIMD.

## Vortex API

`include/vortex/infra/cpu_dispatch.hpp`:

- `CpuFeatures` — bitset of detected x86-64 features (SSE4.2, AVX2, AVX-512F,
  AES-NI, SHA, BMI2, ...), populated at startup (x86-64 `CPUID` implemented).
- `FeatureLevel` — monotone ordering used by the vectorizer and multi-versioner.
- `MultiVersionStub` — self-patching dispatch stub descriptor.
- `HardwareExtensionTable` — maps `extension.hardware.*` UGB capabilities to
  codegen lowering hooks.
