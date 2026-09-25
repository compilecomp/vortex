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

- [x] LDPT code-range reservation: place patch arenas within ±2 GB of published
      code so the skeleton primary path becomes a direct `call rel32`
      (docs/ldpt.md section 1); profile-driven IC guard strengthening at J1
      instantiation
- [x] Light SoN graph construction from UGB + profiles
- [x] Pass pipeline (21 passes) with budget enforcement and graceful degradation
- [x] Linear scan register allocation
- [x] OSR entry/exit between T0/J1/J2
- [x] DoD: cliff-removal benchmark suite — strict J2 > J1 > T0 on the call-cliff
      shape (the tier's target: J1 routes callees through T0 dispatch, J2
      inlines), and the floor T0 > J2·2 / T0 > J1·2 on the direct-loop shape
      (J1's typed stencils are its best case; J2's residual guard cost there is
      an M3 arith/guard-fusion + phi-coalescing item); deopt to T0 correct under
      profile violation; 136 tests green in Release and ASan/UBSan

## Emission scheduling note (M2 review fix)

Inlined graphs break the "node id order is a topological order" assumption:
the splice appends callee definitions behind caller consumers. `plan_emission`
(now in `include/vortex/j2/regalloc.hpp`) computes the per-block
definition-before-use order once and it drives BOTH the register allocator's
position numbering (`emission_positions`) and the emitter's walk — a value
read after a call is guaranteed an interval crossing the call's safepoint, so
it lives in a callee-saved register or a spill slot (Rule 78). The splice also
wires the callee's first effect node onto the caller's effect chain
(Rule 55: side-effect order is part of the pass contract).

## M2 review round (subagent audit — all findings fixed)

Two independent reviewers audited the M2 delta (emission/allocation core;
pipeline/deopt/tests). Verdicts: FIX-REQUIRED (4 blockers, 6 majors). Fixed
and locked by the Rule-121 regression pack in tests/test_j2.cpp. A third
verification pass re-audited every fix (10/11 VERIFIED) and caught one
incomplete arm — the constant-folding compare still materialized smi 0/1 —
now fixed and locked (`j2_folded_constants_use_canonical_words`), plus: the
builder refuses unsigned Div/Rem with a named error (T0 has no M0 handler),
VN's cross-origin soundness argument documented, stale frame comment fixed.
Findings:

- fused-Mul overflow deopt was dead (unpatched `jo` + self-compare roundtrip)
  — `j2_fused_mul_overflow_deopts_like_t0`
- SHL wrapped silently where T0 traps (Rule 110/ADR-005) —
  `j2_shl_out_of_range_traps_like_t0` (also pins the wrap-in-range parity)
- boolean constants were smi 0/1 instead of the canonical 0xB/0xF words —
  `j2_bool_constants_canonical_words`
- Div/Rem were not safepoints AND passed their DivSignedness aux as the
  helper's op id (every division executed as op 0, op 0 = Add.Any) —
  `j2_div_helper_call_keeps_live_values`
- guard merging crossed the splice origin line (a caller guard could prove a
  spliced guard, dropping the callee's exact deopt state) —
  `j2_spliced_guard_keeps_own_deopt_record`
- stage 16 (IC specialization) was dead code (the IC table was never handed
  to the BuiltGraph) — wired, `j2_mono_field_site_specializes_and_stays_parity`
- edge-copy ordering vs flag-fused compares (true-edge copies ran before the
  compare read its inputs; phi intervals now cover the predecessor block and
  the compare stages first), value numbering now validates hits against the
  dominator chain, Kahn cycle fallback fails loudly (Rule 76), field
  accesses write safepoint homes/GC maps, dead guards no longer emit deopt
  records, `PipelineStats` is exposed on `J2Code` (Rule 120 telemetry)
  — `j2_kill_switch_inline_off_keeps_parity_and_telemetry`,
  `j2_budget_refusal_names_the_reason`

## M3 — J3 adaptive full optimizing JIT

- [ ] No-capture deopt at safepoint polls re-runs the whole method in T0
      (effects committed before the poll re-execute — M1-parity contract;
      RBPD's region-capture machinery replaces it, Rules 41/113)
- [ ] Executed OSR-entry parity test with a T0 register snapshot (the M2
      test pins the stub's presence and offset; executing it lands with the
      J3 tiering driver that produces real mid-loop snapshots)
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
