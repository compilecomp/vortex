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

Status in this milestone: API complete (`include/vortex/j4/`), deterministic
pipeline engine contract-stubbed (see `docs/roadmap.md`).
