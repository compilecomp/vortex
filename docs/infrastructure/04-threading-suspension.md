# Infra 4 — Advanced Threading & Suspension System

The guest language needs a concurrency model, and the GC/JIT need to stop threads
safely.

## Mechanisms

- **M:N green thread scheduler.** A work-stealing scheduler mapping millions of
  guest UGB coroutines/fibers onto a fixed pool of OS threads.
- **Asymmetric thread suspension.** To pause a thread for GC or deopt without
  expensive OS signals, the runtime uses handshake/paging tricks: it `mprotect`s
  the thread's stack guard pages; the next time guest code touches the stack, it
  traps into the runtime, which safely yields the thread.
- **Safepoint polling via hardware.** Instead of emitting `CMP`/`JMP` polls in
  J3/J4 code, the runtime uses memory protection: a dedicated polling page is
  marked read-only when a GC is needed; the next `LOAD` from the polling page
  triggers a trap, acting as a zero-overhead safepoint.

## Vortex API

`include/vortex/infra/threading.hpp`:

- `GreenThread`, `FiberContext`, `WorkStealingPool` — M:N scheduler types.
- `HandshakeManager` — per-thread handshake state, request/acknowledge protocol.
- `GuardPageSuspender` — stack guard page based suspension.
- `SafepointPollingPage` — protection-flip polling page configuration.
- `SuspensionKind` — GC, deopt, profiling, debugger.

Contract-stubbed in this milestone; T0 already executes cooperative safepoint
polls at backward branches so the polling contract has a consumer today.
