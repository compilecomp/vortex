# J2 — Fast Optimizing JIT

J2 removes the first performance cliff between the stencil baseline and the full
optimizer. It builds a **lightweight Sea-of-Nodes graph** — not the full heavyweight
CIOG pipeline — and compiles quickly enough to run on many methods.

```text
bytecode + profiles
    -> light Sea-of-Nodes graph
    -> fast optimizations
    -> machine code
```

J2 may use a simplified CIOG for direct calls and monomorphic inline sites, but
does not perform deep polyvariant optimization.

---

## 1. Compile budget

Small but real:

```text
graph node cap
inline depth cap
inline site cap
loop pass cap
vectorization disabled or minimal
no exhaustive fixed point
linear-scan regalloc only
```

If J2 exceeds budget it must degrade gracefully: stop optimizing and emit the
current graph, keep J1 code for cold regions, mark the method for J3 later. J2 must
never become a large compile.

---

## 2. Pass pipeline (21 passes)

1. light graph construction
2. canonicalization
3. constant folding
4. constant propagation
5. SCCP
6. dead code elimination
7. local value numbering
8. limited global value numbering
9. strength reduction
10. null check elimination
11. simple range analysis
12. guard strength reduction
13. redundant guard removal
14. direct-call inlining
15. monomorphic speculative inlining
16. small IC specialization
17. basic block layout improvement
18. linear scan register allocation
19. instruction selection
20. peephole optimization
21. code emission

---

## 3. Inlining policy

```text
inline only:
    direct calls,
    monomorphic call sites,
    small callees,
    shallow depth,
    low guard cost
```

Avoids polyvariant explosion, deep context-sensitive inlining, cross-module
inlining, large CIOG expansion.

---

## 4. Speculation policy

J2 uses profile data conservatively. It specializes int32/int64/float operations,
monomorphic field loads/stores, monomorphic calls, non-null values with strong
evidence, and bounds checks with obvious ranges. It keeps fallbacks close.

---

## 5. Deopt

J2 supports full deopt to T0/J1, lightweight partial deopt for outlined slow
paths, OSR exit, and guard patching. It does not need the full RBPD machinery
everywhere but must be compatible with it.

---

## 6. Role

```text
J1: no real optimization
J2: fast, local, cheap optimization
```

This gives many methods better native code without paying the J3 compile cost.

Status in this milestone (M2): **complete** — the light SoN builder
(`src/j2/graph_builder.cpp`), the pass pipeline with Rule-54 contracts,
kill-switch bitmask and node-cap budget stop (`src/j2/passes.cpp`), linear
scan with safepoint-aware callee-saved preference and home slots
(`src/j2/regalloc.cpp`), and the x86-64 emitter (`src/j2/fast_jit.cpp`) are
implemented and tested (tests/test_j2.cpp): differential parity T0/J2 across
the supported opcode surface, state-exact deopt under profile violation,
two-frame inlined deopt, OSR parity, budget degradation, kill switches, and
the cliff-removal benchmarks. Emission follows `plan_emission`'s
definition-before-use schedule shared with the allocator (see the M2 note in
docs/roadmap.md); linear-scan-only allocation and the caps in section 1 hold.
