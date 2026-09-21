# Vortex Architecture

This document is the entry point to the Vortex design specification. Vortex is a
production-grade just-in-time compilation stack for a managed guest language,
expressed in C++26. It is built from five execution layers, one portable bytecode
standard, one optimizer IR, one partial-deoptimization mechanism, one garbage
collector, and nine infrastructure systems. Every layer is specified in its own
document under `docs/`, and every layer has a complete C++26 API surface under
`include/vortex/`.

---

## 1. The execution stack

```text
T0  speculative register interpreter      [not counted as a JIT tier]
J1  no-IR stencil baseline JIT            [escape the interpreter]
J2  fast optimizing JIT (light SoN)       [remove the first cliff]
J3  adaptive full optimizing JIT          [SoN + CIOG + RBPD, budgeted]
J4  max deterministic optimizing JIT      [no budget, no search, fixed point]
```

A two-tier design (baseline + full optimizer) creates a hard performance cliff: the
jump in both compile latency and code quality is too large. Vortex uses four JIT
tiers so that each step is small in compile cost, small in code-quality delta, and
early in OSR opportunity. Methods flow upward through deterministic thresholds and
flow downward only through region-scoped deoptimization or explicit demotion.

### 1.1 Tier contracts

| Tier | Name | IR | Compile budget | Purpose |
|------|------|----|----------------|---------|
| T0 | Speculative register interpreter | bytecode only | none | startup, profiling, adaptive rewriting, ICs, OSR source |
| J1 | Stencil baseline JIT | **no IR** | near-instant | escape the interpreter; stencils and superstencils |
| J2 | Fast optimizing JIT | light Sea-of-Nodes | small, capped | remove the first cliff; cheap but real optimization |
| J3 | Adaptive full optimizing JIT | full SoN + CIOG | budgeted, aggressive | main production tier; PEA, vectorization, RBPD |
| J4 | Max deterministic optimizing JIT | persistent SoN + CIOG | **no artificial budget** | peak quality for very hot, stable code |

The hard invariants that define the tier system:

1. There are exactly four JIT tiers: J1, J2, J3, J4.
2. The T0 interpreter is not counted as a JIT tier.
3. J1 uses stencils and superstencils and builds no IR of any kind (no AST, no CFG,
   no SSA, no SoN, no MIR, no LIR).
4. J2 uses a lightweight Sea-of-Nodes graph and a simplified CIOG.
5. J3 uses the full Sea-of-Nodes IR and the full CIOG.
6. J4 uses persistent Sea-of-Nodes and CIOG and reuses IR across recompiles.
7. J3 is budgeted; reaching a budget must still produce good code, never a failure.
8. J4 has no artificial compile, inline, node, pass-count, or code-size budget.
9. J4 is deterministic and non-search-based: no superoptimization, no stochastic
   pass ordering, no phase-order search, no ML-driven search, no unbounded
   equality-saturation exploration.
10. All tiers support OSR upward where useful; OSR exists between every pair of
    JIT tiers, not only from the interpreter.
11. All tiers preserve deopt metadata and precise GC root maps.
12. J3 and J4 use full region-based partial deoptimization.
13. Performance cliffs are prevented by graduated compile cost, graduated code
    quality, overlapping optimization coverage between adjacent tiers, background
    J4 compilation, and profile stability gates.

### 1.2 Graduation model

```text
Compile cost:   T0: zero   J1: tiny   J2: small   J3: medium   J4: unbounded (background)
Code quality:   T0: interpretive   J1: stencil   J2: fast-opt   J3: aggressive   J4: peak
Stability gate: J1: hotness   J2: basic type/branch stability
                J3: stable ICs and call targets
                J4: phase stability + low deopt rate
```

Adjacent tiers overlap in capability so no hard feature cliff exists:

```text
inlining:        J1 none/minimal  -> J2 direct/small -> J3 budgeted speculative -> J4 unbounded deterministic
escape analysis: J1 none          -> J2 local EA     -> J3 full PEA/temporal    -> J4 exhaustive + interprocedural
vectorization:   J1 none          -> J2 minimal      -> J3 budgeted VEC/SLP     -> J4 full VEC/SLP, no caps
```

### 1.3 Transitions

Upward transitions (`T0->J1`, `T0->J2` via OSR for very hot loops, `J1->J2`,
`J2->J3`, `J3->J4`) use background compilation, OSR where possible, atomic entry
patching, profile transfer, IC state transfer, and deopt metadata generation.

Downward transitions (`J4->J3`, `J3->J2`, `J2->J1`, `J1->T0`) happen on deopt
storms, guard failure spikes, class-hierarchy invalidation, profile instability,
memory pressure, or code-cache eviction, and always preserve useful profile data.

J4 compiles in the background while J3 executes, publishes hot regions first, and
patches entries atomically; it never blocks execution.

---

## 2. Component map

```text
                       +-----------------------+
 guest program  -----> | guest language        |   YOUR frontend: any language
 (any language)        | frontend              |   that emits UGB (see
                       +----------+------------+   docs/porting.md; the Mini
                                  | lowers to        reference frontend is an
                                  v                  example, not a product)
                       +-----------------------+
                       |  UGB module (.ugb)    |   Universal Guest Bytecode
                       |  + verifier           |   register-based, capability-based
                       +----------+------------+
                                  | loads
                                  v
+-------------+        +-----------------------+        +------------------+
| tiering     | <----> |  T0 interpreter (vm/) | -----> | profile + IC data|
| policy      |        |  speculative register |        | per-site records |
+------+------+        +----------+------------+        +--------+---------+
       |                          |                                   |
       | promote                  | OSR entry                         | feeds
       v                          v                                   v
+------+------+   +------+  +---+---+   +-------+   +-------+  +----+----+
| code cache  |   |  J1  |  |  J2   |   |  J3   |   |  J4   |  | compile |
| (infra/08)  |<--|stenci|->|light  |->|full   |->|persist|<-| threads |
+------+------+   |  l   |  | SoN   |   | SoN+  |   | SoN+  |  | (infra) |
       ^          +------+  +-------+   | CIOG  |   | CIOG  |  +---------+
       |                                +---+---+   +---+---+
       |        +----------------+          |           |
       |        | RBPD (deopt/)  |<---------+-----------+
       |        | region tables  |
       |        +-------+--------+
       |                | guard failure
       v                v
+------+----------------+------+
| ICGC (gc/)                   |
| young/old/humongous, SATB,   |
| Brooks, TLAB, card table     |
+------------------------------+

Infrastructure systems (infra/):
 1 dependency/invalidation   6 observability/tooling
 2 security (W^X, CFI, ...)  7 CPU dispatch / intrinsics
 3 snapshots / AOT           8 executable memory / I-cache
 4 threading / suspension    9 power / thermal
 5 FFI / ABI translation
```

---

## 3. UGB: Universal Guest Bytecode

UGB is the portable contract between guest-language frontends and the Vortex
runtime. It is register-based, versioned, capability-based, profile-friendly,
deopt-aware, GC-aware, and extensible. The JIT never needs to know the guest source
language.

Key properties (full specification in `docs/ugb.md`):

1. Virtual registers `v0..vN` with register classes (ANY, I8..I64, F32/F64, REF,
   CLOSURE, COROUTINE, TYPE, SHAPE, METHOD, ...).
2. Canonical semantic instructions plus speculative specialized forms with
   mandatory fallback paths (`Add.Any` -> `Add.I32` with `fallback_site_id`).
3. Tokens (type, shape, field, method, dispatch descriptors) instead of any fixed
   object model — supports class-based OO, prototype-based OO, dynamic scripting,
   functional, systems, managed, async, and concurrent languages.
4. Stable site IDs for every profiled/IC-guarded construct; profile and IC records
   are part of the standard.
5. Precise exception state, suspend state, deopt state, and GC root maps per PC.
6. Capability negotiation (`ugb.pack.*`), namespaced extensions
   (`extension.<language>.<feature>`), conformance levels
   (`UGB-Core-Interpreter` ... `UGB-MaxOpt`).
7. Verification is mandatory before execution: opcode validity, register
   def-before-use, control-flow and handler validity, token validity, capability
   satisfaction, GC reference safety, suspend/deopt state consistency.

---

## 4. The optimizer IR: Sea-of-Nodes + CIOG

The optimizing tiers share one IR design (full document in `docs/ir-son-ciog.md`):

- **Sea-of-Nodes**: nodes connected by data, control, effect, and guard
  dependencies; no rigid basic blocks; implicit SSA; explicit effect tokens model
  memory dependencies; guards are first-class nodes that produce proven values.
- **CIOG (Call/Inline/Outline Graph)**: an overlay that tracks call sites, inline
  candidates and decisions, outline regions, context keys, specialization variants,
  deopt regions, barrier regions, and cold regions. Every `OutlineRegion` becomes a
  partial deopt region.

J2 runs a fast, capped version of this pipeline; J3 runs the full budgeted pipeline
(60 passes listed in `docs/tier-j3.md`); J4 runs the deterministic full pipeline to
fixed point with persistent IR.

---

## 5. RBPD: Region-Based Partial Deoptimization

Deoptimization in Vortex is never all-or-nothing (full document in
`docs/deopt-rbpd.md`):

- A **region** is an independently compilable unit with its own entry, exit
  continuation, register allocation, GC map, deopt record, escape set, failure
  counter, bytecode PC range, and tier id.
- On a guard failure only the failing region is invalidated; only the region's
  **escape set** is materialized; recovery recompiles the region, jumps to the
  successor region, or falls back tier-by-tier.
- Regions can merge and split adaptively based on correlated or isolated failures.
- Effect safety: effects before a failing guard are never rolled back, so effect
  chains constrain which code may follow an escapable guard.

---

## 6. ICGGC: Incremental Concurrent Generational GC

The collector (full document in `docs/gc-icggc.md`) is incremental, concurrent,
generational, region-based in the old generation, pause-bounded, compaction-
capable, and JIT-integrated:

- young generation with TLAB bump allocation and evacuation;
- old generation with concurrent SATB marking, remembered sets, concurrent sweeping
  and evacuation;
- Brooks-pointer read barriers for concurrent compaction with JIT-proven elision;
- card-table write barriers with inline/outlined forms and escape-analysis-driven
  elimination;
- an incremental pacer that adapts to allocation rate and pause budget;
- tight metadata interchange: JIT emits GC maps, register masks, frame descriptors;
  GC exposes heap base, card table base, TLAB pointers, region tables, phase state.

---

## 7. The nine infrastructure systems

| # | System | Spec | Header |
|---|--------|------|--------|
| 1 | Dependency & invalidation ("CHA engine") | `docs/infrastructure/01-dependency-invalidation.md` | `include/vortex/infra/dependency.hpp` |
| 2 | Security & exploit mitigation | `docs/infrastructure/02-security.md` | `include/vortex/infra/security.hpp` |
| 3 | Startup snapshots & AOT pipeline | `docs/infrastructure/03-snapshots-aot.md` | `include/vortex/infra/snapshot.hpp` |
| 4 | Advanced threading & suspension | `docs/infrastructure/04-threading-suspension.md` | `include/vortex/infra/threading.hpp` |
| 5 | Native interop (FFI) & ABI translation | `docs/infrastructure/05-ffi.md` | `include/vortex/infra/ffi.hpp` |
| 6 | Observability, debugging & tooling | `docs/infrastructure/06-observability.md` | `include/vortex/infra/observability.hpp` |
| 7 | Hardware intrinsics & CPU dispatch | `docs/infrastructure/07-cpu-dispatch.md` | `include/vortex/infra/cpu_dispatch.hpp` |
| 8 | Executable memory & I-cache manager | `docs/infrastructure/08-icache-codecache.md` | `include/vortex/infra/code_cache.hpp` |
| 9 | Power & thermal management | `docs/infrastructure/09-power-thermal.md` | `include/vortex/infra/power.hpp` |

---

## 8. End-to-end execution flow

```text
cold start:      method loaded -> T0 executes -> T0 profiles -> T0 rewrites
                 speculative bytecode -> ICs update -> counters increment
baseline:        method hot -> J1 stencil compile -> instantiate + patch ->
                 atomically installed; execution continues in J1
optimized:       profiles stable -> J2/J3 compile -> SoN/CIOG -> optimize ->
                 schedule -> outline -> regalloc -> emit -> install
peak:            J3 very hot + stable -> J4 enqueued in background ->
                 persistent IR -> deterministic full pipeline -> hot regions
                 patched first -> full method upgraded when ready
spec failure:    guard fails -> trap -> identify region -> materialize escape
                 set -> partial recompile / successor region / tier fallback
GC:              TLAB allocation -> young collection when full -> concurrent
                 old-gen marking -> barriers log updates -> read barriers
                 support compaction -> JIT uses GC maps + deopt metadata
```

---

## 9. Final architectural invariants

1. T0 uses speculative register bytecode.
2. J1 has no IR and uses stencils/superstencils.
3. J2 and J3/J4 use Sea-of-Nodes; J3/J4 add the full CIOG.
4. Every optimized trap site aligns with a partial deopt region boundary.
5. Partial deopt invalidates only one region.
6. Only escape-set values are materialized during partial deopt.
7. Effects are tracked by effect tokens.
8. Non-idempotent effects cannot be reordered across partial deopt boundaries.
9. GC barriers are integrated with escape analysis and effect scheduling.
10. All compiled code has precise GC maps and deopt metadata.
11. Compilation is concurrent and multi-threaded.
12. IC updates are atomic.
13. Code installation is safe, barriered, and patchable.
