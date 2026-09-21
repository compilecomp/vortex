# RBPD — Region-Based Partial Deoptimization

The deopt core of Vortex. Principle: **deopt is not all-or-nothing**. When a guard
fails, only the failing region is invalidated; the rest of the optimized method
remains live.

---

## 1. Region definition

A region is an independently compilable unit. Each region has:

```text
entry point
exit continuation
private register allocation
private GC map
private deopt record
escape set
failure counter
bytecode PC range
tier identifier
```

Region kinds:

```text
HOT_REGION           COLD_REGION          DEOPT_REGION
BARRIER_REGION       SLOW_PATH_REGION     INLINE_FALLBACK_REGION
UNCOMMON_TRAP_REGION EXCEPTION_REGION
```

---

## 2. Escape set

For every region, compute the values that escape it:

```text
values consumed outside the region
values live across region exit
pointer roots needed by GC
values needed by the continuation stub
```

Only escape-set values are materialized during partial deopt. This is the property
that makes deopt cheap: the cost is proportional to the region's escape set, not to
the whole frame.

---

## 3. Partial deopt record

```text
PartialDeoptRecord:
    region_id
    bytecode_resume_pc
    escape_value_count
    escape_locations[]
    successor_region_id
    tier_fallback_target

EscapeLocation:
    virtual_register
    location_type: REG | STACK | CONST
    physical_register | stack_offset | constant
```

---

## 4. Deopt protocol

When a guard traps:

```text
1. Identify region from code pointer.
2. Load region descriptor.
3. Load partial deopt record.
4. Increment region failure count.
5. Materialize escape-set values.
6. Decide recovery path.
```

Recovery paths:

```text
A. Recompile only this region.
B. Jump to successor region.
C. Fall back to baseline.
D. Fall back to interpreter.
E. Use shared deopt trampoline.
```

---

## 5. Partial recompile

If region failure is recoverable:

```text
extract region subgraph
strengthen failed guard or remove speculation
recompile region only
patch call sites to new region
mark old region stale
```

No full method recompile.

---

## 6. Region merging and splitting

Adaptive region management:

- adjacent regions repeatedly deopt together -> merge;
- large region with one failing guard -> split around it;
- region too small (call overhead dominates) -> merge;
- region too large for efficient partial deopt -> split.

J4 additionally tunes regions continuously: merge correlated regions, split
regions with isolated failures, move guards to region boundaries, re-outline slow
paths, regenerate continuation stubs, reduce escape sets, improve composite GC
maps.

---

## 7. Effect safety

Effects before a failing guard are **not** rolled back. Therefore:

- non-idempotent effects must not be placed after an escapable guard unless proven
  safe;
- external calls require guard placement before the call;
- stores with visible side effects require effect-chain validation;
- partial deopt regions must respect effect boundaries.

---

## 8. GC interaction

During partial deopt:

- lower frames use their own GC maps;
- the failed region contributes escape-set roots;
- the GC scanner uses a composite root view;
- no full stack scan is required unless the fallback requires it.

Every optimized trap site aligns with a partial deopt region boundary — this is a
hard architectural invariant.
