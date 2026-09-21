# Infra 6 — Observability, Debugging & Tooling System

You cannot ship a black box. The JIT must dynamically expose its internals to OS
tools.

## Mechanisms

- **JIT profiler interface (perf/VTune/ETW).** The JIT dynamically writes
  `jitdump` files or ETW events, mapping dynamically generated machine-code
  addresses to UGB bytecode source lines, so `perf record` shows exact
  guest-language hotspots.
- **Debugger integration (GDB/LLDB).** The JIT implements the GDB JIT Interface
  API, dynamically registering DWARF debug info, symbol names, and GC root maps so
  breakpoints and stack traces work seamlessly in native debuggers.
- **Managed sanitizers.**
  - *AddressSanitizer for UGB:* insert shadow-memory checks in J2/J3 to catch
    guest-language buffer overflows.
  - *ThreadSanitizer for UGB:* instrument UGB atomic and field operations to
    detect guest-level data races.
- **Deterministic replay.** Recording thread scheduling, IC updates, and OSR
  transitions to a log, allowing exact time-travel replay of non-deterministic JIT
  bugs.

## Vortex API

`include/vortex/infra/observability.hpp`:

- `JitDumpWriter` — `perf-jitdump` ELF container writer contract (mmap/MMAP_EVENT/
  CODE_LOAD records).
- `GdbJitInterface` — dynamic DWARF registration/unregistration contract.
- `SanitizerPolicy` — managed ASan/TSan instrumentation switches.
- `ReplayRecorder` — deterministic replay event log (scheduling, IC updates, OSR
  transitions).
- `TierEventSink` — structured tier/deopt/compile event stream used by the `vx
  stats` tool.
