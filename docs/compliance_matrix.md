# Compliance Matrix — UGB/VORTEX Compiler Laws & Architecture Specification

**Status:** Authoritative mapping from every law to its enforcement mechanism and current compliance state.
**Review cadence:** every release (Rule 134). This matrix was last audited against the full 137-rule spec.

## Status legend

- **COMPLIANT** — enforced mechanically or by implemented code today.
- **PARTIAL** — implemented for the shipped scope (M0: T0 + UGB + reference frontend); the unshipped remainder is contractually pinned to a milestone.
- **PLANNED** — the owning subsystem is contract-stubbed; the law binds its design (see `docs/roadmap.md`). A rule marked PLANNED with no milestone reference is a defect — file it.

## Part I–II: Architecture & UGB Laws

| Rule | Status | Enforcement / Notes |
|---:|:---|---|
| 1 | COMPLIANT | JIT executes UGB only; the reference frontend lowers Mini→UGB (`src/frontends/reference`). No AST reaches any engine. |
| 2 | COMPLIANT | No pass hardcodes guest semantics; language knowledge is confined to frontends + capability declarations + runtime hooks. |
| 3 | COMPLIANT | `ugb::Capability` / `CapabilitySet`; per-method `required_capabilities`; engine advertises via `engine_advertised_capabilities()`; `Interpreter::check_capabilities` + verifier reject unknown/unsupported safely (tests: `unsupported_capability_is_rejected_safely`, `unknown_capability_rejected_by_verifier`). |
| 4 | COMPLIANT | Typed/speculative opcodes carry canonical fallbacks executed inline on speculation failure (`note_speculation_failure` + canonical handler in T0); deopt/fallback metadata is part of the RBPD region contract (R37). |
| 5 | COMPLIANT | Hot paths keyed by tokens/ids: `(klass_id, token)` FlatHashMap caches for virtual calls and field slots; string lookups exist only on cold cache-fill paths. Lint job forbids node containers in hot trees. |
| 6 | COMPLIANT | `ExtensionRef` with namespace grammar `extension.<language>.<feature>`; verifier rejects malformed names and version 0; unknown extensions are not executable (EXT_OP requires the Extensions capability). |
| 7 | COMPLIANT | `ugb::verify_module` runs before any execution: opcodes, registers, defined-before-use, control-flow targets, call windows, tokens, capability ids, extension grammar. Artifacts are re-verified after decode (tests: `verifier_rejects_*`). |
| 8 | COMPLIANT | `make_site_id(method_id, instruction_index)`; `UGBMethod::site_id`; profile slots, IC slots and tier-transition records carry it (test: `site_ids_are_stable_and_injective`). |
| 9 | COMPLIANT | Binary codec and the text assembler both use bounds-checked readers (sentinel token past end of stream); malformed input rejected with `DecodeErrorInfo`; version check rejects incompatible majors/minors (Rule 10); nothing executes unverified (Rule 7). Fuzzing harness: see Rule 122 (PLANNED for CI harness). |
| 10 | COMPLIANT | Format v2 records UGB major/minor, capability mask, extension table, guest language id, runtime ABI version, metadata schema version. Decoder rejects incompatible versions explicitly (test: `binary_roundtrip_preserves_capabilities_and_versions`). |

## Part III: Tiering & Pipeline Laws

| Rule | Status | Enforcement / Notes |
|---:|:---|---|
| 11 | PARTIAL | T0 executes the full core path (control, arithmetic, calls, objects, arrays, guards, safepoints). Extended ISA forms (`Const.String`, `Swap`, unchecked/unsigned variants, `TailCall.*`) verify and then reject at dispatch with an explicit error — safe degradation (R3/R29), never misexecution; they map to handlers in M1. Switch-fallback absence is EXC-002. |
| 12 | COMPLIANT | J1 is contract-stubbed to stencil semantics only; `include/vortex/j1` defines stencils/superstencils/IC slots with no IR types (docs/tier-j1.md §2 forbids IR). |
| 13 | PLANNED | J2 light SoN on the shared `ir::Graph` with explicit budget struct (roadmap M2). Budget fields are declared in docs/tier-j2.md. |
| 14 | PLANNED | J3 full SoN + CIOG + RBPD (roadmap M3); budgets listed in docs/tier-j3.md. |
| 15 | PLANNED | J4 no-budget + safety constraints (cancellable, incremental publication) pinned in docs/tier-j4.md (roadmap M4). |
| 16 | PLANNED | J4 determinism: fixed pass order, termination metrics (`FixedPointState` fingerprint exists). Enforced by determinism tests at M4. |
| 17 | PARTIAL | Tier capability overlap is documented per tier (docs/tier-j*.md "overlap" sections); OSR counters and requests fire in T0 today; cross-tier OSR arrives with J1/J2 (M1/M2). |
| 18 | COMPLIANT (M0) / PARTIAL | One semantic contract: T0 is the only executing engine in M0 and is the differential baseline; once J1+ land, the differential CI (Rule 119) is the enforcement mechanism. |
| 19 | PLANNED | J2/J3/J4 share `ir::Graph`, `CiogOverlay`, `deopt::RegionTable` — same IR node definitions and pass contracts; tier differences expressed as budgets/pass lists. |
| 20 | COMPLIANT (M0) / PARTIAL | Profiles (ProfileSlot) only bias rewrite decisions; they never change semantics — canonical fallbacks preserve behavior (R4). |
| 21 | PARTIAL | Guard-based optimization only in M0 (no static-proof engine); proof machinery is designed in docs/ir-son-ciog.md §6 and gated to M3+. |
| 22 | COMPLIANT | Tiering is a pure function of invocation/backedge/IC/deopt counters (`TieringPolicy::evaluate`); no wall-clock inputs (test-verified via determinism of tier decisions). |
| 23 | PLANNED | Async compile + atomic install: Rule 87/88 machinery (W^X publish) exists; the compile queue binds it in M1+. |
| 24 | PLANNED | Frozen snapshots: profile snapshot APIs are declared in docs/tier-j2.md; enforcement at M2. |
| 25 | PLANNED | Compilation cancellation: dependency graph supports invalidate-during-compile (`src/infra/dependency.cpp`); compiler binding at M2+. |
| 26 | PLANNED | Recompile throttle/backoff: declared in docs/tier-j3.md §8; enforced when J3 lands. |
| 27 | PARTIAL | OSR request path + counters + observer exist in T0; full state transfer (Rule 27 list) is a J2 deliverable (M2) with deopt-safety per R40. |
| 28 | COMPLIANT | Every transition decision (OSR request, promotion) is recorded to the bounded transition ring (`tier_transitions()`) + TieringObserver callbacks. Silent transitions are impossible in T0. |
| 29 | COMPLIANT | Compiler failures return Result; the engine degrades with diagnostics. No compile-path construct can abort a guest program; EXC-001 documents the OOM caveat. |

## Part IV–V: Speculation, Guards, RBPD, IR Laws

| Rule | Status | Enforcement / Notes |
|---:|:---|---|
| 30 | PARTIAL | Every T0 speculation (typed ops, shape loads) has an explicit failure path; full guard-metadata records (R31) bind to J3 (M3). |
| 31 | PLANNED | Guard metadata struct declared in docs/deopt-rbpd.md §4; enforced at M3 via IR verifier checks. |
| 32 | PARTIAL | ProfileSlot carries counts/failures/branch stats; decay/variance/confidence land with the profile snapshot work (M2). |
| 33 | PARTIAL | Dependency graph (`infra/dependency`) implements registry/edges/invalidation; assumptions registered by compilers at M2+. |
| 34 | COMPLIANT (M0) / PARTIAL | Every T0 specialization has an inline generic fallback (Rule 34's core); clone-level fallback machinery is J3+ (M3). |
| 35 | PLANNED | Guard optimization pass list (docs/deopt-rbpd.md §7); J3+ (M3). |
| 36–39 | PLANNED | RBPD region metadata, partial deopt isolation, exact state reconstruction: contracts pinned in `include/vortex/deopt/rbpd.hpp` + docs/deopt-rbpd.md (M3/M4); region tables + failure protocol data structures exist. |
| 40 | COMPLIANT (M0) / PARTIAL | T0 can execute every method by law (R11); full-deopt metadata completeness is verified when J-tiers land (R86). |
| 41–45 | PLANNED | Effect safety across partial deopt, FrameState completeness, deopt throttling, guard healing: RBPD milestone (M3/M4); docs/deopt-rbpd.md pins the contracts. |
| 46 | COMPLIANT | IR has explicit effect classes: pure/effectful/guard node-kind partitions (`is_pure`/`is_effectful`/`is_guard`) + effect-token chaining on the graph. |
| 47 | COMPLIANT | `ir::Graph` separates data / control / effect (in + reverse outs) / guard inputs as distinct edge classes. |
| 48 | COMPLIANT | `NodeId = uint32_t`; all edges are indices; raw pointer edges are forbidden and lint-checked (test: `graph_edges_survive_realloc`). |
| 49 | COMPLIANT | All identifiers are interned into token tables before optimization; the IR uses integer ids only. |
| 50 | COMPLIANT | `FlatHashMap` (open addressing, linear probe) is the hot-path map; node containers are banned in hot trees by lint. |
| 51 | COMPLIANT | `SparseSet` for dataflow sets (DCE reachability); `std::set`/`unordered_set`/`vector<bool>` banned in hot trees by lint. |
| 52 | COMPLIANT | `SmallVector<T, N>` for IR operand lists, IC poly entries and guard lists; used by `ir::Node`. |
| 53 | PLANNED | SoA bulk passes arrive with the vectorization/SLP work (M3+); no current pass iterates AoS hot arrays at scale. |
| 54 | PARTIAL | Pass contract struct (required/produced/invalidated/budget/determinism) is specified in docs/ir-son-ciog.md §7; T0's "passes" (rewriters) satisfy it informally; enforced for all J-tier passes at M2+. |
| 55 | COMPLIANT (M0) / PARTIAL | T0 rewriters are idempotent (rewrite to the same opcode is a no-op); fixed-point termination metrics for J4 = `FixedPointState` (M4). |
| 56 | COMPLIANT (M0) / PARTIAL | Compilation inputs are explicit (bytecode, flags, profile, target); no RNG in any pipeline; nondeterminism sources list = empty and lint-checked for M0. |
| 57 | COMPLIANT | The one process-global table (T0 dispatch) is immutable after single-writer init (ADR-002); no pass reads hidden mutable globals. |
| 58 | COMPLIANT | Generic passes contain no arch conditionals; target knowledge lives in `infra/cpu_dispatch` + codegen interfaces. |
| 59 | PARTIAL | T0 knobs (adaptive rewriting, profiling, thresholds, depth, array cap) are config + kill-switchable; env-var surface and per-pass gates for J-tiers land with the compile queue (M1+). |
| 60–61 | PLANNED | J4 non-search + termination metrics (M4); `FixedPointState` graph fingerprint exists as the seed. |
| 62–64 | PLANNED | CIOG overlay structures exist (CallNode/InlineSite/OutlineRegion); full CIOG tracking, outline-as-RBPD-unit validation, persistent IR reuse: M3/M4. |

## Part VI: Systems Implementation Laws

| Rule | Status | Enforcement / Notes |
|---:|:---|---|
| 65 | COMPLIANT | No `throw` anywhere in `src/`/`include/` (lint-checked); all fallible APIs return `Result<T> = std::expected`; guest exceptions are runtime values (R66) — no native unwinding anywhere. |
| 66 | COMPLIANT | Guest exception opcodes (TRY_BEGIN/TRY_END/THROW) are control-flow values; no native exception propagation exists. |
| 67 | COMPLIANT (M0) / PARTIAL | T0 dispatch loop: zero steady-state allocation (frame pool, span args, no per-call vectors); compiler phases use `Arena`; deopt materialization allocation rules bind at M3. |
| 68 | COMPLIANT | `-fno-rtti` on all targets; explicit kind enums everywhere (`NodeKind`, `ExprKind`, `StmtKind`, `Op`); lint forbids dynamic_cast/typeid. |
| 69 | COMPLIANT | No shared_ptr/function in hot trees (lint-checked); ownership is explicit; static dispatch throughout. |
| 70 | COMPLIANT | `Result<T>` + `fail()`/`ok()` monadic style; error branches compile to expected-state checks, no exception tables on hot paths. |
| 71 | PARTIAL | `[[likely]]`/`[[unlikely]]` are used where empirically justified (dispatch error paths); full PGO-driven hint validation awaits benchmarks (M5). |
| 72 | COMPLIANT | Every T0 threshold/limit is a named, documented config constant (`max_call_depth`, `max_array_length`, rewrite thresholds, tiering thresholds); `kInvalidInstructionIndex`/`kNoSite`/`kNoNode` named sentinels. |
| 73 | PARTIAL | No pass hardcodes cache line/SIMD width (none exist yet); CPU parameters are queried via `infra/cpu_dispatch`; cost models are M3+ deliverables bound by this rule. |
| 74–75 | PLANNED | Heuristic validation benches are an M5 gate (perf gates, Rule 126); no unvalidated heuristic is enabled in a production pipeline before then. |
| 76 | COMPLIANT | Fallbacks/errors are never silent: every failure carries a Diagnostic; T0 records tier decisions (R28); `vx stats` surfaces counters. |

## Part VII–VIII: GC, Code Cache, Security Laws

| Rule | Status | Enforcement / Notes |
|---:|:---|---|
| 77 | PARTIAL | Generational heap + card table + TLAB exist; incremental/concurrent marking-sweep phases land per docs/gc-icggc.md (M2+). |
| 78 | PARTIAL | T0 frames are precisely rooted (register frames are scanned; values are tagged); JIT GC maps are a required output of every tier compile (R86) — enforced at M2+. |
| 79–80 | COMPLIANT (M0) / PARTIAL | Every reference store in T0 executes the card-marking barrier; elision requires proof (none elided today). |
| 81 | COMPLIANT (M0) / PARTIAL | Safepoint polls execute at backward branches (`SAFEPOINT_POLL` handler + hook); bounded latency via handshake infra (M2). |
| 82 | COMPLIANT | Allocation failure returns a guest-visible runtime error; the VM never aborts on ordinary allocation failure (test-verified behavior of TLAB slow paths). |
| 83 | COMPLIANT | Mutators allocate through TLAB; global sync only on slow paths. |
| 84–86 | PLANNED / PARTIAL | Non-moving young gen in M0 (identity stable by construction); moving-gen identity, weakref/finalizer state and compile-output GC metadata bind to the concurrent GC milestone. |
| 87 | COMPLIANT | `WritableCodeMemory`: mmap PROT_READ\|PROT_WRITE → mprotect PROT_READ\|PROT_EXEC on publish; no write-after-publish; no simultaneous W+X window. |
| 88 | PARTIAL | Publication uses the W^X flip; release/acquire publication of entry points is exercised by the J-tier install path (M1+). |
| 89–91 | PLANNED | Concurrent patching safety, epoch reclamation, code-cache budgets: infra 2/8 chapters pin the contracts; enforced with their milestones. |
| 92 | PARTIAL | Constant blinding infrastructure exists (`infra/security`); JIT-spraying PoC tests are a CI security gate (R129, planned). |
| 93 | PARTIAL | Non-exec stack/heap by default on Linux; CET/CFI integration is documented as platform-dependent in docs/infrastructure/02-security.md. |
| 94 | PARTIAL | Generated code contract exists in docs/tier-j1.md §6 (approved runtime entrypoints only); enforced from J1 landing (M1). |
| 95–98 | PLANNED / PARTIAL | Dependency recording exists (`infra/dependency`); AOT/snapshot manifests: `infra/snapshot` declares the compatibility manifest fields (R97) — verification enforced at the AOT milestone. |
| 99 | COMPLIANT | No locks on T0 hot paths: IC updates are single-mutator by contract (documented in module.hpp), no global locks exist in allocation/barrier/poll paths. |
| 100 | PARTIAL | Epoch reclamation for code/metadata lands with code installation (M1+); IR reclamation is arena-bulk today. |

## Part IX–X: FFI and Guest Fidelity Laws

| Rule | Status | Enforcement / Notes |
|---:|:---|---|
| 101–105 | PLANNED | FFI is capability-gated (R3: engine does not advertise FFI) and contract-stubbed (`infra/ffi`); the laws bind the M5+ implementation; no FFI optimization exists to violate them. |
| 106 | COMPLIANT (Mini) | `docs/guest-semantics.md` registers the Mini reference language oracle: observable behavior is defined by that document + the golden test suite; T0 is the differential baseline until a J-tier exists. |
| 107 | COMPLIANT (M0) | No optimization reorders/duplicates/deletes effects; the only rewrites are same-semantics opcode specializations with canonical fallbacks. |
| 108 | PARTIAL | Constant folding does not exist in T0 (nothing to fold incorrectly); the purity requirement binds IR passes at M2+. |
| 109 | PARTIAL | Dynamic features are not optimized (M0 has none beyond virtual calls, which dispatch through live klass tables every call — no assumption baked); unsupported capabilities reject safely. |
| 110 | COMPLIANT | Numeric semantics are exact and regression-tested: overflow traps (no wrap, no UB via overflow builtins), IEEE div/zero and NaN flow, saturating F64→I64, named conversion semantics (tests: `f64_*`, `smi_overflow_*`, `mul_overflow_*`, `neg_smi_min_*`). |
| 111–116 | PLANNED / PARTIAL | Escape analysis, guest exception state, frame materialization, suspension, tooling hooks: bound to J3/RBPD milestones; nothing in M0 can violate them (no such optimizations exist). |
| 117 | COMPLIANT | No hash/address/randomness assumptions baked anywhere; T0 makes no address-identity optimizations. |
| 118 | COMPLIANT | Static typing is absent from M0; annotations do not justify unsafe optimization because no static optimization exists. |

## Part XI: Testing, Observability, Governance Laws

| Rule | Status | Enforcement / Notes |
|---:|:---|---|
| 119 | PARTIAL | 65-test suite covers UGB/T0/containers/IR/deopt/infra incl. numeric edges and capability rejection; cross-tier differential CI activates with the tiers; guest-oracle differential = golden `examples/*.mini` outputs. |
| 120 | PLANNED | Golden IR tests per pass: gated to the pass pipelines (M2+); golden UGB encode/decode + disassembly tests exist today. |
| 121 | COMPLIANT | Bug fixes in this repo ship with regression tests (see the numeric P1 fixes: 7 new tests; the verifier call-window fix carries its own test). |
| 122 | PARTIAL | Bytecode decode is fuzz-hardened by construction (bounds-checked Reader, version gates); a standalone fuzz harness + corpus is a CI deliverable (M1). |
| 123 | PARTIAL | CI runs Debug/Release; ASan/UBSan configs are wired in CI (this release); TSan target for the concurrency suite lands with the compile queue (M1+); MSan where supported is tracked. |
| 124 | PLANNED | GC/deopt stress modes: forced-GC flag exists in the heap; full stress matrix binds to the concurrent GC milestone. |
| 125 | PLANNED | Code-install concurrency tests: with the code installation milestone (M1+). |
| 126 | PLANNED | Perf gates (startup, warmup, p99, deopt rate, code size): M5 harness; no release before then claims perf numbers. |
| 127 | PARTIAL | Structured counters exist (InterpStats, transition ring, IC stats, alloc stats); the telemetry schema doc consolidates them (docs/observability in infra 6); privacy: no source/user data is recorded. |
| 128 | PARTIAL | Replay inputs for T0 decisions: bytecode hash + config + profile are capturable via `vx stats`/`dis`; full compile-replay artifacts bind to the compiler pipeline (M2+). |
| 129 | PARTIAL | W^X is structurally enforced; lint + artifact-validation tests exist; JIT-spray PoCs and cache-exhaustion tests are CI security deliverables (M1+). |
| 130 | PARTIAL | `docs/guest-semantics.md` + `docs/porting.md` track the Mini subset, known divergences (none) and unsupported features; per-guest compatibility registers are added per new frontend. |
| 131 | PARTIAL | T0 specializations (adaptive rewriting, profiling) have kill switches in config; the full per-optimization gate list (inlining, PEA, LICM, …) applies as those optimizations land (M2+), each with a flag at birth. |
| 132 | COMPLIANT | `docs/exception-register.md`: every deviation is registered with rule id, reason, owner, risk, mitigation, telemetry and expiry; silent bypasses fail the lint review checklist. |
| 133 | COMPLIANT | This matrix + `tools/lint/compliance.sh` (Rules 48/50/65/68/69 mechanically checked) + CI jobs; rules without enforcement are marked PLANNED with an enforcement plan, never silent. |
| 134 | COMPLIANT | This document; reviewed each release. |
| 135 | COMPLIANT | `docs/adr/` — ADR-001 exceptions policy, ADR-002 dispatch, ADR-003 IR index edges, ADR-004 capability negotiation, ADR-005 numeric semantics; new architectural decisions require a new ADR (review checklist). |
| 136 | PARTIAL | No external dependencies at all (zero-dependency build); hermetic CI with pinned runner toolchain; reproducible-artifact hashing lands with AOT (R97). |
| 137 | COMPLIANT | Docs updated in the same change as behavior (UGB spec updated with format v2, guest semantics added with the numeric changes — this release). |

## Release audit checklist

1. `cmake --build build && ctest` — all tests green.
2. `cmake --build build --target compliance_lint` — structural laws green.
3. CI matrix green (Debug/Release + ASan/UBSan + lint job).
4. Any rule whose status changed is reflected here in the same PR (R137).
5. Exception register entries within expiry (R132).
