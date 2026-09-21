# Optimizer IR — Sea-of-Nodes + CIOG

The optimizing tiers (J2 light, J3 full, J4 full+persistent) share one IR design.

---

## 1. Sea-of-Nodes

Nodes are connected by four kinds of dependencies: **data**, **control**,
**effect**, and **guard**. There is no strict basic-block-centric CFG during
optimization. SSA is implicit. Code can move without rigid basic-block
constraints, which is what makes global code motion, early/late scheduling, and
PEA natural.

### 1.1 Node classes

Pure data nodes (no side effects):

```text
Const  Parameter  Add  Sub  Mul  And  Or  Xor  Shift  Compare  Phi  Select
```

Effectful nodes (chained by effect tokens):

```text
Load  Store  Call  Allocate  WriteBarrier  ReadBarrier  Safepoint  ExternalCall
```

Control nodes:

```text
If  Region  Loop  End  Return
```

Guard nodes (produce proven values):

```text
TypeGuard  NullGuard  BoundsGuard  ShapeGuard  ClassGuard
OverflowGuard  InterfaceGuard
```

```text
guard(value, condition) -> proven_value
```

### 1.2 Effect tokens

Effect tokens model side effects explicitly:

```text
effect_in -> node -> effect_out
effect0 -> Store A -> effect1 -> Store B -> effect2
```

This enables precise load/store scheduling, write-barrier hoisting and
elimination, call motion constraints, memory dependency refinement, guard
placement relative to effects, and safe partial-deopt boundaries. Independent
stores may reorder; aliasing-dependent stores may not.

---

## 2. CIOG — Call/Inline/Outline Graph

The CIOG overlays the Sea-of-Nodes graph and tracks:

```text
call sites, inline candidates, inlined bodies, outline regions,
context keys, specialization variants, deopt regions,
barrier regions, cold regions
```

CIOG node types:

```text
CallNode  InlineSite  OutlineRegion  ContextKey
SpecializationKey  DeoptRegion  ColdRegion  BarrierRegion
```

### 2.1 CallNode

```text
CallNode:
    call_site_id, bytecode_pc
    receiver_profile, argument_profiles
    candidate_callees[], execution_count, failure_count
    specialization_key
    inline_decision: INLINE | DONT_INLINE | DEFER
                   | SPECULATIVE_INLINE | POLYVARIANT_INLINE
```

### 2.2 InlineSite

```text
InlineSite:
    caller_region, callee_graph, parameter_bindings,
    return_projection, guard_chain, context_key, deopt_continuation
```

### 2.3 OutlineRegion

An OutlineRegion is a subgraph extracted from the main compiled body. Kinds:

```text
OUTLINE_DEOPT          OUTLINE_COLD           OUTLINE_BARRIER
OUTLINE_SLOW_PATH      OUTLINE_INLINE_FAILURE OUTLINE_EXCEPTION
OUTLINE_UNCOMMON_TRAP
```

**Every OutlineRegion becomes a partial deopt region** (see
`docs/deopt-rbpd.md`).

---

## 3. The optimizer pipeline (stages)

The stages below constitute the full pipeline; J2 runs a capped subset, J3 the
budgeted full set, J4 the deterministic full set to fixed point.

1. **Graph building** — decode bytecode; create data nodes for registers, effect
   nodes for loads/stores/calls, guard nodes for speculative assumptions, call
   nodes from profile data; attach IC metadata, branch probabilities, type
   feedback, allocation-site metadata.
2. **Canonicalization** — constant folding, identity simplifications, strength
   reduction, algebraic rewrites, boolean simplification, compare normalization,
   redundant phi cleanup, effect token normalization.
3. **SCCP + dead node elimination** — sparse conditional constant propagation;
   unreachable node removal; dead pure/effect node elimination where safe.
4. **GVN** — identical expressions, congruent operations, redundant loads with
   proven aliasing, redundant comparisons/arithmetic/guard conditions.
5. **Escape analysis** — full EA and partial EA using connection analysis,
   allocation-site tracking, call-graph escape effects, effect chains, and CIOG
   interprocedural information. Results: stack allocation, scalar replacement,
   lock elision, barrier elimination, lazy materialization, object slicing,
   deferred field initialization.
6. **Guard optimization** — redundancy elimination, subsumption, hoisting,
   sinking, clustering, strength reduction, fusion, patch-point placement, region
   boundary alignment.
7. **Inlining** — via CIOG: adaptive, speculative, polyvariant, context-sensitive,
   receiver-type-directed, cross-function virtualization, speculative
   devirtualization, TCO where possible; respecting inline budget, code size,
   register pressure, guard cost, deopt risk, callee hotness, context explosion.
8. **Cross-inline propagation** — constants, types, shapes, receiver classes,
   argument constraints, effect dependencies; remove newly redundant guards and
   IC checks.
9. **Loop optimizations** — LICM, peeling, unrolling, range analysis, bounds check
   elimination, induction variable optimization, fusion, fission, interchange.
10. **Vectorization and SLP** — loop vectorization, SLP, loop-aware SLP, packed
    arithmetic and memory ops, vector compare/select, vectorized BCE.
11. **Memory optimization** — redundant load elimination, store-to-load
    forwarding, load hoisting, store scheduling, alias refinement, write/read
    barrier optimization, field and layout specialization.
12. **Scheduling** — early/late code motion, effect-aware, guard-aware,
    latency-aware, register-pressure-aware, cache-locality clustering.
13. **Outlining** — extract cold, deopt, barrier stub, exception, slow allocation,
    failed-inline, uncommon-trap regions; each becomes an independent partial deopt
    region.
14. **Register allocation** — linear scan primary, graph coloring for hot methods,
    phi-web/SSA-web coalescing, spill optimization, rematerialization, register
    mask caching for GC.
15. **Instruction selection** — pattern matching: architecture patterns, fused
    compare/branch, conditional moves, addressing modes, SIMD, barriers, atomics,
    switch lowering.
16. **Machine-level optimization** — peephole, scheduling, branch hints, code
    alignment, hot/cold layout, NOP sleds, patchpoint placement, relocation
    optimization.
17. **Emission** — hot code, cold regions, deopt regions, barrier regions,
    continuation stubs, GC maps, deopt records, region descriptors, metadata
    tables.

---

## 4. Guard system

A guard is a SoN node:

```text
Guard:
    checked_value
    condition
    assumption
    failure_target
    proven_value_output
```

Kinds: `NullGuard, TypeGuard, ClassGuard, ShapeGuard, BoundsGuard, OverflowGuard,
InterfaceGuard, ArgumentGuard, ReceiverGuard, ArrayLengthGuard, WriteBarrierGuard,
ReadBarrierGuard, LockGuard`.

**Elimination methods**: dominance-based redundancy elimination, type propagation,
range propagation, escape-driven elimination, inline-boundary propagation,
loop-invariant guard hoisting, guard subsumption.

**Strength reduction**:

```text
full class check  -> compressed klass compare -> tag bit test -> pointer identity
null check        -> test reg, reg / compare against zero
bounds check      -> unsigned compare
interface check   -> IC word compare
shape check       -> hidden class pointer compare
```

**Fusion**: multiple guards on the same value fuse into a single compound
condition, or become branchless (`cmov`/select) when profitable.

**Motion**: hoist to dominator, sink to first use, cluster related guards, move
above pure code, move across control when effect-safe, move across inlines when
proven safe.

**Healing**: if a guard fails rarely, patch it in place (strengthen/weaken the
check, adjust the IC) and avoid full deopt; if it fails repeatedly, trigger
region-based partial deopt of only the failing region.

---

## 5. Metadata produced

```text
MethodMetadata:  bytecode, profile table, IC table, constant pool,
                 baseline code pointer, optimized code table, region table,
                 deopt table, GC maps, frame descriptors
RegionTable:     region_id, code_start, code_end, descriptor_offset,
                 gc_map_offset, deopt_record_offset, continuation_stub,
                 failure_count
FrameDescriptor: stack_size, callee_saved_mask, gc_root_mask,
                 deopt_layout, interpreter_mapping
```

GC maps are compressed with bitvectors, run-length encoding, delta encoding,
shared frame descriptors, and interned masks.
