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

- [x] RBPD region-capture deopt: safepoint polls carry FrameState records
      (escape set = live frame, resume pc = the poll pc) — the captured
      resume replaces the M1 no-capture whole-method T0 rerun, which remains
      only as the Rule-40 fallback; suspension events are recorded WITHOUT
      advancing failure counters (suspension is not speculation loss)
- [x] Interop message protocol dispatch: POLY_READ/POLY_WRITE lowered into
      class-guarded raw field accesses under the NativeObjects-port license
      (the dispatch vanishes — docs/interop-protocol.md section 6);
      POLY_EXECUTE/POLY_SEND dispatch in T0 through the registered vtable
      (EXECUTE/SEND graph lowering lands with J4 devirt); capability-gated
      load (`.requires interop_messages` + module CAP_INTEROP_* mask,
      verified at load); language registry with klass binding
- [x] Cross-Language Escape Analysis (proven form): merged-graph EA after
      inlining, single-block scalar replacement with Rule-39 rematerialization
      descriptors (consts embedded, live fields from the deopt window),
      escape-summary publication with graph-hash identity + monotonic
      weakening; speculative XLEA with runtime G1-G5 guards is J4 (the
      profile-license machinery is not in this tier yet — docs/xlea.md 3/5)
- [x] Full SoN + CIOG construction: the shared builder + the CIOG overlay
      (CallNode per call site, InlineSite per spliced body, OutlineRegion
      per deopt region, context keys in discovery order)
- [x] Full 60-stage budgeted pipeline (three waves) — every named stage runs
      exactly once: transforming, delegated (J2 scalar core / shared
      backend), or NotApplicable with a NAMED domain reason (no fake passes;
      the mapping is in docs/tier-j3.md and j3/passes.hpp). Real new stages:
      RLE/store-to-load forwarding, range analysis, guard dominance
      elimination, EA, scalar replacement, deferred field init, CIOG,
      loop identification + LICM, interprocedural summaries
- [x] RBPD region formation, escape sets, recovery paths (5-path protocol),
      Rule-43 throttle computation (site/region/method thresholds,
      blacklist verdicts), region merge/split/replace; per-region failure
      counters through the run path. Scope note: M3 computes the verdicts
      and records Blacklisted state; CONSUMING the verdict (entry-trap
      gating of blacklisted regions in the tiering driver) is M4/J4 —
      tracked there.
- [x] DoD: partial deopt invalidates ONLY the failing region (region failure
      counter + neighboring region state preserved — tested at the table
      level and through the J3 run path); J3 == T0 parity on the optimizer
      suites; the J3 > J2 perf delta is tracked for the benchmark suite
      (M3 landed the machinery; the tuned measurement harness is M4 — the
      M2 cliff benchmark already shows the tier stack healthy)
- [x] Executed OSR-entry parity test with a T0 register snapshot (the M2
      test pins the stub's presence and offset; M4 executes it — the
      backedge hook produces a real mid-loop snapshot and the compiled
      OSR entry continues it to parity, `m4_osr_entry_parity_with_t0_snapshot`)

## M4 — J4 max deterministic optimizing JIT

- [x] Persistent IR store with incremental reuse (`j4::PersistentIrStore`;
      content-addressed identity, drop-on-stale — `j4_persistent_ir_*`)
- [x] Deterministic fixed-point engine with termination invariants (graph
      hash stability or a repeated-hash cycle — never an iteration cap;
      inline depth bounded only by the deopt-frame ABI capacity)
- [x] Background worker + incremental region publication (`j4::J4Worker`,
      stepwise capture/advance/emit/publish; Rule-15 cancellation honored
      at the next step boundary)
- [x] DoD: bit-identical compiled output across repeated compiles of the
      same input (determinism test — which surfaced and fixed the baked
      deopt-record addresses: emission now carries relocations, publish
      patches them); no search (the stage order is the fixed J3 driver;
      inline growth terminates on the call-graph cycle guard); J3 keeps
      running during a J4 compile (worker-step interleaving test)
- [ ] CEP&CC 0.1 adoption (backbone standard, adopted at M3 with a dated
      waiver — `.cep/baseline.md`, `.cep/waivers/`): migrate the SEV1
      findings to zero (nine-field file headers, seven-field function
      blocks — hot trees first, reusing PERF_CONTRACT facts as the cost
      answers), repair the severity-2 backlog incrementally, and flip
      `tools/lint/cep_lint.sh` to enforced in CI. **Batch 1 done (M4):
      the J4 tree + its test/bench files ship CEP&CC-complete and the j4
      tree is SEV1-clean; the remaining trees continue batch by batch.**
      Zero severity-0 at adoption: the CEM-26 hot-code bans and CEP&CC
      agree on the core.
- [x] M3 review follow-ups (12-review minors, all landed in M4): golden-IR
      tests for the per-pass TUs (the 60-stage telemetry is now asserted —
      and the goldens surfaced the Smi-min sign bug that had silently
      disabled folding, plus the range-flow-through-guards gap, tracked
      for J4); a cross-origin (splice-stamp) guard-dominance regression
      test through the J3 arm; the RBPD Rule-43 throttle verdict consumed
      in the tiering driver (speculation gate + the policy's verdict
      parameter — escalation refusal and Rule-40 demotion); `publish_j3`
      summary exercised end-to-end (the harness was publishing a default
      summary — fixed); the remaining `std::unordered_map`/`std::function`
      in src/j3 swapped to the shared containers (Rule 50/69 hygiene);
      degenerate zero-width region metadata dropped (regions now carry the
      real [resume_pc, next_pc) bytecode span); CIOG overlay
      idempotent-return values clarified (one meaning per return, no
      renumbering on re-application); Smi-bound spellings unified on the
      canonical TaggedValue bounds.
- [x] M4 review round (two independent reviewers; verdicts SHIP /
      FIX-REQUIRED, all findings dispositioned): the executed
      `m4_osr_entry_parity_with_t0_snapshot` and the deopt-frame ABI
      capacity pair (record side builds TO kMaxDeoptFrames; refusal side
      keeps the guard beyond capacity out of the graph — the original
      single test was unsatisfiable by construction); the cyclic
      call-graph termination test the DoD was missing, which exposed and
      fixed a real M1-era defect — `helper_invoke_token` resolved callees
      positionally although the token pool is first-reference ordered, so
      forward-referenced callees (mutual recursion) silently called the
      WRONG method (now resolves by name like T0, Rule 18; backward
      references are bit-identical); `find_reusable` drops half-dead
      entries so `reused_ir` telemetry cannot lie (Rule 120); the
      determinism test warms the token cache symmetrically so the byte
      compare covers spliced shapes; the tiering-gate test pins the
      gated-phase transition contract; the `.cep/baseline.md` batch-1
      delta narrative corrected to the verified per-file numbers; spec
      drift fixed (tier-j4.md 12.1 hash construction, 12.3 publish/swap
      wording, 12.5 enforcement wording; j4 ADR-002 citations moved to
      the 12.3 driver model). Deferred minors, tracked for later
      milestones: a cumulative inline-depth stat in J4Stats (the
      refusal-side capacity test cannot assert splice depth directly);
      the advance()-before-capture misuse returning a progress-shaped
      value; graph_hash not observing effect/control-only rewires
      (deterministic and terminating, narrower than the 12.2 prose —
      spec note owed); bench_tiers timing J3 through run_j3 while J2/J4
      use raw entry stubs (asymmetry documented, not unified); a direct
      CIOG idempotent-return unit test (currently covered indirectly by
      fixed-point stability).

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
