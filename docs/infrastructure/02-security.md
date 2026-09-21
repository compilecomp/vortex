# Infra 2 — Security & Exploit Mitigation System

JITs are the primary attack surface in modern runtimes. Vortex enforces hardware
and OS-level security invariants around all emitted code.

## Mechanisms

- **W^X (Write XOR Execute) memory manager.** Code memory is never simultaneously
  writable and executable. The JIT writes to RW pages, issues a memory barrier,
  and flips to RX via `mprotect`/`VirtualProtect`.
- **JIT spraying prevention.** Constant blinding: XOR embedded 64-bit constants
  with a random per-method mask and XOR them back at runtime, so attackers cannot
  embed executable shellcode in immediate operands.
- **Control-Flow Integrity (CFI).** Emit `BTI` (Branch Target Identification) on
  ARM or `IBT` (Indirect Branch Tracking) on x86 for all valid call/branch
  targets.
- **Shadow call stacks / PAC.** Sign return addresses and function pointers using
  ARM PAC or software shadow stacks to prevent ROP.
- **Spectre/Meltdown mitigations.**
  - *Index masking:* auto-mask array indices with `AND` masks to prevent
    speculative out-of-bounds reads.
  - *Speculative barriers:* insert `LFENCE`/`CSDB` after critical branch
    predictions.
  - *Site isolation:* JIT code from different security origins (web workers,
    iframes) never shares a code cache or CPU cache sets.

## Vortex API

`include/vortex/infra/security.hpp`:

- `WritableCodeMemory` — RW/RX pageFlip manager (implemented on POSIX
  `mprotect`); every code publication path in the runtime goes through it.
- `ConstantBlinding` — per-method XOR masks, `blind`/`unblind`, policy control.
- `CfiPolicy` — BTI/IBT emission flags per target.
- `ShadowStack` / `PacMode` configuration types.
- `SpectrePolicy` — index masking, barrier insertion, site isolation knobs.

The W^X manager and constant blinding are implemented and unit tested in this
milestone; CFI/PAC/Spectre emission hooks are wired into the codegen contracts.
