# Roadmap

Vortex follows a spec-first, milestone-driven plan. **M0 is complete**: the full
design specification, the complete C++26 API surface, the UGB toolchain (text
assembler, encoder/decoder, disassembler, verifier), the reference frontend (Mini —
a minimal example of a language port, not a product language), the working
T0 interpreter, the foundational runtime (tagged values, object model, TLAB, card
table), the W^X security manager, the dependency/invalidation engine, the handle
table, the CPU feature detector, the code-cache segmentation, the test harness,
and CI.

Every milestone below lists its definition-of-done as verifiable test criteria.

---

## M0 — Spec-first repo (this milestone, complete)

- [x] Full design specification (`docs/`)
- [x] Complete public API surface (`include/vortex/`)
- [x] UGB: opcode ISA, module builder, binary encoder/decoder, text assembler,
      disassembler, verifier
- [x] T0 speculative register interpreter: frames, tagged vregs, mono/poly/mega
      ICs, profile slots, adaptive rewriting, superinstructions, card-marking
      write barriers, TLAB allocation fast path, safepoint polls, tier handoff
      hooks
- [x] Reference frontend (Mini): lexer, parser, UGB emitter — an example of a
      language port, kept under `frontends/reference/`
- [x] Runtime foundations: tagged values, object model (klasses, shapes,
      objects, arrays), tiering policy
- [x] Infra: W^X code memory, constant blinding, dependency/invalidation graph,
      handle table, CPU feature detection (CPUID), code cache segmentation
- [x] `vx` driver: `run`, `dis`, `stats` commands
- [x] Test harness + unit/integration tests + CI workflow

## M1 — J1 stencil baseline JIT (complete)

- [x] x86-64 assembler core with relocations (needed by stencils)
- [x] Stencil corpus for the core opcode set (typed + generic forms) — 68
      templates assembled once at corpus build; instantiated by memcpy + patch
- [x] Superstencil promotion from hot bigrams (`promote_superstencils`)
- [x] IC embedding + guard patch-point NOP sleds (`patch_ic_guard`; profile-driven
      guard strengthening at instantiation arrives with the M2 plumbing — the
      always-slow default is correct for every receiver)
- [x] Baseline deopt records + OSR entry stubs (per backward-branch target;
      `J1OsrFn` materializes all vregs from a T0 register-file snapshot)
- [x] DoD: guest programs compiled by J1 produce identical observable results to
      T0 (fib/arithmetic/fields/objects parity tests); golden machine-code pins;
      compile latency per method under budget (10 ms CI-safe bound)

## M2 — J2 fast optimizing JIT

- [ ] LDPT code-range reservation: place patch arenas within ±2 GB of published
      code so the skeleton primary path becomes a direct `call rel32`
      (docs/ldpt.md section 1); profile-driven IC guard strengthening at J1
      instantiation

- [ ] Light SoN graph construction from UGB + profiles
- [ ] Pass pipeline (21 passes) with budget enforcement and graceful degradation
- [ ] Linear scan register allocation
- [ ] OSR entry/exit between T0/J1/J2
- [ ] DoD: cliff-removal benchmark suite; J2 > J1 > T0 on hot-loop microbenchmarks;
      deopt to T0 correct under profile violation

## M3 — J3 adaptive full optimizing JIT

- [ ] Interop message protocol dispatch: POLY_EXECUTE/POLY_READ/POLY_WRITE/
      POLY_SEND lowering, vtable registration, capability-gated load
      (docs/interop-protocol.md)
- [ ] Cross-Language Escape Analysis: merged-graph EA after cross-language
      inlining, scalar replacement, escape-summary publication/consumption,
      guard emission G1–G5 (docs/xlea.md)

- [ ] Full SoN + CIOG construction
- [ ] Full 60-pass budgeted pipeline (three waves: scalar core, inlining/CIOG,
      loops/vector/backend)
- [ ] RBPD region formation, escape sets, continuation stubs, partial recompile
- [ ] DoD: partial deopt invalidates only the failing region (test: region failure
      counter + neighboring region state preserved); J3 > J2 on optimizer suites

## M4 — J4 max deterministic optimizing JIT

- [ ] Persistent IR store with incremental reuse
- [ ] Deterministic fixed-point engine with termination invariants (graph hash,
      lattice state)
- [ ] Background worker + incremental region publication
- [ ] DoD: bit-identical compiled output across repeated compiles of the same
      input (determinism test); no search (audit: pass drivers contain no
      enumeration); J3 keeps running during J4 compile

## M5 — ICGGC concurrent engine

- [ ] Concurrent SATB marker with incremental pacing
- [ ] Concurrent sweep + evacuation with Brooks forwarding
- [ ] GC-map-driven precise stack scanning for JIT frames
- [ ] DoD: mutator pause p99 under budget on allocation-stress suite; no missed
      SATB logs (assertion mode); compaction correctness under FFI pinning

## M6 — Infrastructure completion

- [ ] Snapshots/AOT (M3): heap snapshot writer/reader, shared code cache, PGO-AOT
- [ ] Threading (M4): M:N work-stealing scheduler, guard-page suspension,
      safepoint polling page
- [ ] FFI (M5): trampoline generator, unified unwinding
- [ ] Observability (M6): jitdump, GDB JIT interface, managed sanitizers,
      deterministic replay
- [ ] CPU dispatch (M7): multi-versioning stubs, hardware extension lowering
- [ ] Code cache (M8): background sweeper/compactor, NUMA placement, huge pages
- [ ] Power (M9): thermal listener + governor wiring
- [ ] Dependency engine (M1): native patching integration with code cache

## Post-M6

- ARM64 backend (AAPCS64, BTI/PAC)
- Additional capability packs (coroutines, atomics, vector) exercised by the
      reference frontend and documented for external language ports
- Conformance test suite for UGB levels
