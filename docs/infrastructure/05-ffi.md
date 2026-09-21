# Infra 5 — Native Interop (FFI) & ABI Translation System

Guest code must call C/C++/Rust, and native code must call back into the JIT.

## Mechanisms

- **Trampoline generator.** Dynamically generates machine-code stubs that
  translate the UGB calling convention (virtual registers, tagged values) to the
  native OS ABI (System V / ARM AAPCS) and back.
- **Object pinning & handle tables.** Because the ICGGC moves objects
  concurrently, native code cannot hold raw pointers. The FFI system provides a
  **handle table** (indirection pointers) or **pinning regions** (temporarily
  forbidding the GC from moving specific objects during the native call).
- **Unified stack unwinding.** Integrates the JIT's table-driven unwinder (for
  exceptions/partial deopt) with the OS native unwinder (`libunwind` / DWARF
  `.eh_frame`), allowing a C++ exception to safely unwind through J4 optimized
  guest frames.

## Vortex API

`include/vortex/infra/ffi.hpp`:

- `AbiKind` (SystemV x86-64, AAPCS64), `ForeignSignature` descriptors.
- `TrampolineGenerator` — UGB <-> native ABI stub synthesis contract.
- `HandleTable` — scope-checked global/weak/local handles (implemented; unit
  tested with allocation/free scopes).
- `PinningRegion` — RAII pin scopes the GC consults.
- `UnifiedUnwinder` — JIT frame walker + native unwinder bridge contract.
