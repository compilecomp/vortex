# J4 — Max Deterministic Optimizing JIT

J4 is the final peak tier for very hot, stable code: peak code quality, **no
artificial compile budget**, **no search-based compilation**, a deterministic
exhaustive pipeline, whole-hot-program optimization, persistent IR, and
incremental reoptimization.

---

## 1. Design

```text
uses:
    full Sea-of-Nodes,
    full CIOG,
    persistent IR,
    whole-hot-method optimization,
    whole-hot-program context where available,
    region-based partial deopt,
    advanced code layout,
    high-quality register allocation,
    deterministic fixed-point optimization
```

---

## 2. What "no compile budget" means

J4 does not stop early because of a time, node, inline, graph-size, code-size,
pass-count, polyvariant-context, unroll, or vectorization budget. It continues
optimizing until:

```text
the deterministic pipeline reaches completion
or no further legal profitable transformation remains
or the graph is fully lowered/emitted
```

The goal is peak code quality for very hot stable code.

## 3. What "not search-based" means

J4 is not a superoptimizer. It does **not** use brute-force instruction search,
enumerative peephole search, stochastic optimization, genetic algorithms,
reinforcement learning, phase-order search, random pass ordering, exhaustive
inline-tree enumeration, ML-driven compile search, or equality-saturation
exploration as an unbounded search engine.

J4 uses deterministic pass ordering, deterministic rewrite rules, deterministic
cost models, dependence-driven scheduling, fixed-point iteration with termination
rules, canonical context hashing, and profile-directed but deterministic
specialization. It may be expensive, but it is predictable.

---

## 4. Compile model

J4 compiles in the background while J3 executes:

```text
J3 code runs
    -> J4 worker builds persistent optimized graph
    -> J4 applies full pipeline
    -> J4 emits hot regions first
    -> J4 patches entries atomically
    -> J4 replaces J3 incrementally
```

J4 never blocks execution. If J4 is slow, J3 remains active.

---

## 5. Optimization scope

Everything from J3, without budget cutoffs, plus:

1. exhaustive canonicalization
2. repeated fixed-point GVN/PRE/DCE
3. full cross-function virtualization
4. full receiver type propagation
5. full interprocedural escape analysis
6. full temporal PEA
7. full lazy materialization
8. full polyvariant specialization
9. full context-sensitive cloning
10. cross-module/runtime LTO where metadata exists
11. global code placement
12. hot/cold region splitting
13. barrier outlining and elimination
14. deopt region tuning
15. guard elimination to the maximum provable extent
16. loop transformations without artificial unroll caps
17. vectorization and SLP with full dependence analysis
18. instruction scheduling with full latency model
19. high-quality register allocation
20. phi-web/SSA-web coalescing
21. spill/rematerialization optimization
22. code alignment and cache clustering
23. metadata compression and prefetch layout
24. persistent IR reuse for future recompiles

---

## 6. Inlining and polyvariant specialization

J4 inlining has no artificial budget but still uses deterministic cost rules:

```text
inline if: profile confidence high, callee body beneficial,
    guard cost acceptable, code locality improves,
    deopt risk manageable, context can be canonicalized
```

Explosion is avoided with context canonicalization, polyvariant sharing, call
graph cycle detection, inline tree hashing, speculative inline pruning, and dead
inline elimination — never search-tree exploration.

Polyvariant specialization may generate multiple variants for receiver types,
argument types, constant arguments, caller contexts, shape combinations, branch
profiles, and allocation-site behavior — but only for observed stable contexts,
canonicalized and shared.

---

## 7. Fixed-point rules

```text
repeat until stable:
    canonicalize
    SCCP
    GVN
    DCE
    PRE
    guard simplification
    constant propagation
    effect chain cleanup
```

Stability is measured by graph hash, node count, value-numbering state, constant
lattice state, guard set, and effect-chain identities. No random ordering.

---

## 8. Register allocation and code layout

Linear scan for cold/normal regions; graph coloring for hot regions; phi-web and
SSA-web coalescing; rematerialization analysis; split live-range optimization;
register-pressure-aware scheduling feedback. Still deterministic, still no
stochastic search.

Aggressive layout: hot region clustering, cold region isolation, deopt region
outlining, barrier stub sharing, uncommon trap grouping, I-cache-friendly
ordering, branch target alignment, prefetch-friendly metadata placement.

---

## 9. Partial deopt

J4 uses full RBPD and improves it: merge correlated regions, split regions with
isolated failures, move guards to region boundaries, re-outline slow paths,
regenerate continuation stubs, reduce escape sets, improve composite GC maps, and
patch failing regions without disturbing the rest.

---

## 10. Trigger conditions

J4 compiles only methods that are extremely hot, profile-stable, low-deopt-rate,
phase-stable, important for steady-state throughput, and not rapidly changing in
class hierarchy or call behavior:

```text
J3 execution count high
AND deopt rate low
AND ICs stable
AND branch profiles stable
AND type profiles monomorphic/polymorphic but stable
```

---

## 11. Final tier rules (normative)

1. No artificial compile-time budget.
2. No artificial inline budget.
3. No artificial node budget.
4. No artificial pass-count budget.
5. No search-based optimization.
6. No superoptimization.
7. No stochastic pass ordering.
8. Deterministic transformations only.
9. Deterministic cost models only.
10. Fixed-point iteration must have termination invariants.
11. Profile-directed, but not search-directed.
12. Compiles in background while J3 runs.
13. Can publish code incrementally by region.
14. Must preserve RBPD compatibility.
15. Must preserve GC map/deopt correctness.

Status in this milestone: API complete (`include/vortex/j4/`); the M4
deterministic engine is specified in section 12 below and implemented
against it (spec-first).

---

## 12. M4 engine contract (the deterministic engine, spec-first)

This section is the M4 specification the implementation follows. It is
written before the code (spec-first) and the code must not diverge from it
without a doc change in the same commit.

### 12.1 Persistent IR store (`j4::PersistentIrStore`)

The store keeps the built pipeline-processed graph of a method alive across
compiles so a recompile starts from the previous result instead of from
bytecode:

```text
key        (module_id, method_id)
entry      bytecode_version_hash, graph, escape summary, last fingerprint,
           publication state (draft | published | invalidated)
reuse rule identical bytecode_version_hash  -> reuse the stored graph
           anything else                     -> rebuild from bytecode
```

`bytecode_version_hash` is the FNV-1a walk over the method's UGB bytecode
(graph-hash construction, support/hash.hpp — the same walk the
escape-summary identity uses; docs/xlea.md 4.1). The store
never guesses staleness: any hash mismatch invalidates the entry (graph and
summary are rebuilt; the stale entry is replaced, not patched). Lookups are
keyed — no enumeration (Rule 16). Invalidation is explicit: the dependency
engine's invalidation callback (or a direct call) marks the entry
`invalidated` before a new compile may start (Rule 15: compilation must be
cancellable on invalidation).

### 12.2 Deterministic fixed-point engine (`MaxJit::compile`)

Pipeline (Rule 19 — the same optimizer core as J2/J3):

```text
build (j2::build_graph)
 -> run_j3_pipeline (the full 60-stage J3 driver, no budgets)
 -> fixed-point loop over the J3 driver until the termination rule fires
 -> shared backend emit (j2::emit_optimized, Tier::J4)
```

"No budget" is literal (Rule 15): the J4 driver passes no node cap, no
inline caps, and no pass-count cap — only the Rule-59/131 kill switches
remain, because they are a test/debug affordance, not a budget. The
fixed-point loop therefore cannot use an iteration cap as its stop
condition. The termination rule is:

```text
iterate:
    fp = fingerprint(graph)            // graph hash + node count
    if fp.graph_hash == prev.graph_hash: STABLE   (fixed point reached)
    if fp.graph_hash was seen before:    CYCLE    (deterministic termination:
        keep the state of the first repeat of the current suffix — the run
        is deterministic, so the cycle boundary is deterministic)
    run the pipeline once more
```

Invariants (normative):

1. `fingerprint` is a pure function of the live graph (kind/aux/payload/
   inputs in id order) — never of time, address, or iteration order.
2. Every iteration is monotone-or-neutral per stage (Rule 55); the loop
   terminates because the number of distinct graph hashes is finite and
   both stop edges (STABLE, CYCLE) are reached in finite steps.
3. The seen-hash set is a keyed flat structure (Rule 50), bounded by the
   number of iterations actually performed.
4. Memory exhaustion and external invalidation are the only abort paths,
   both surfaced as named `Result` failures (Rule 76) — never silent
   truncation of the fixed point.

### 12.3 Background worker + incremental region publication (`j4::J4Worker`)

Compile model (docs/tier-j4.md section 4) without pretending M4 has the M6
threading infra: the worker is a stepwise state machine the embedding
driver advances between mutator executions. It holds no interpreter state
and takes no locks, so J3 keeps running while a J4 compile is in progress —
the mutator only ever observes the worker through atomic publication
points.

```text
states: Idle -> Captured -> Optimizing(step...) -> Emitted -> Published
step(): advances exactly one state transition (Optimizing advances one
        fixed-point iteration per step)
publish(): replaces the method's executable and its RBPD regions
        incrementally — per region, through RegionTable::replace_region —
        and hands the new executable to the caller, which performs the
        entry-point swap (one move into its executable slot; between the
        region replacement and the swap the driver treats the pair as one
        quiescent step — single-mutator stepwise publication, spec 12.3's
        driver model)
```

Contract: a `Captured` worker owns a private copy of the graph (from the
persistent IR store); the mutator's J3 execution never reads it until
`publish()`. An invalidation between steps is honored at the next `step()`
(Rule 15: cancellable); the worker then drops the draft and reports the
cancellation by name.

### 12.4 Tiering-driver consumption of RBPD verdicts (M3 follow-up)

The tiering driver installs a per-method speculation gate
(`Interpreter::set_speculation_gate`) backed by the executable's region
table. Promotion becomes a function of hotness AND the Rule-43 verdict:

```text
verdict == Enter | WeakenAssumptions -> hotness promotes as usual
verdict == DowngradeTier | RefuseMethod -> promotion to J2/J3/J4 refused;
           a method already on an optimized tier records a Fallback
           transition to its fallback tier (Rule 40), with the named
           decision as the reason (Rules 28/76)
```

The gate is non-owning and keyed by method id; blacklisted regions stay
gated until the gate owner clears or replaces it (the caller owns expiry,
matching the throttle contract).

### 12.5 M4 DoD (from docs/roadmap.md)

1. Bit-identical compiled output across repeated compiles of the same
   input (determinism test — compares emitted code bytes).
2. No search: the stage order is the fixed J3 driver; the driver contains
   no enumeration, no randomness, no phase-order exploration (audited by
   the determinism test plus the fixed-pipeline assertion in tests).
3. J3 keeps running during a J4 compile (worker-step interleaving test).
4. Executed OSR-entry parity with a real T0 register snapshot.
5. CEP&CC migration: batch 1 (the J4 tree + its test/bench files) ships
   CEP&CC-complete and severity-1 clean; the remaining trees migrate
   batch by batch per `.cep/baseline.md`, and `cep_lint.sh` flips to
   enforced (`CEP_LINT_ENFORCE=1`) when the SEV1 count reaches zero.
