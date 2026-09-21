# J3 — Adaptive Full Optimizing JIT

J3 is the main aggressive optimizer and the steady-state workhorse: high
performance with controlled compile cost.

```text
uses:
    full Sea-of-Nodes,
    full CIOG,
    full profile feedback,
    region-based partial deopt,
    full effect-token scheduling,
    budgeted interprocedural optimization
```

---

## 1. Compile budget

Budgeted to prevent compile storms:

```text
graph node budget          inline budget            polyvariant context budget
loop unroll budget         vectorization budget     register allocation time budget
code size budget           compilation wall-time budget    memory budget
```

If J3 reaches budget limits it must still emit good code, not fail.

---

## 2. Pass pipeline (60 passes)

1. graph building from bytecode/profiles
2. canonicalization
3. SCCP
4. GVN
5. DCE
6. PRE
7. partial subexpression elimination
8. strength reduction
9. algebraic simplification
10. range analysis
11. null check elimination
12. escape analysis
13. partial escape analysis
14. temporal PEA / lazy materialization
15. connection analysis
16. scalar replacement
17. lock elision
18. object slicing
19. deferred field initialization
20. CIOG construction
21. adaptive inlining
22. speculative inlining
23. speculative devirtualization
24. static devirtualization
25. cross-function virtualization
26. receiver type propagation
27. polyvariant specialization
28. context-sensitive inlining
29. interprocedural escape analysis
30. loop identification
31. LICM
32. loop peeling
33. loop unrolling
34. loop fusion/fission
35. loop interchange
36. bounds check elimination
37. induction variable optimization
38. auto-vectorization
39. superword-level parallelism
40. loop-aware SLP
41. guard redundancy elimination
42. guard subsumption
43. guard hoisting
44. guard sinking
45. guard clustering
46. guard strength reduction
47. guard fusion
48. effect-aware scheduling
49. global code motion
50. barrier placement
51. barrier hoisting
52. barrier elimination
53. outline extraction
54. region formation
55. register allocation
56. instruction selection
57. instruction scheduling
58. peephole optimization
59. code layout
60. metadata emission

---

## 3. RBPD in J3

J3 uses full region-based partial deopt. Each outlined region carries:

```text
region descriptor, escape set, continuation stub,
partial deopt record, GC map, failure counter
```

Guard failures invalidate only the failing region. See `docs/deopt-rbpd.md`.

---

## 4. Role

J3 is the balanced high-performance tier: budgeted but aggressive, profile-directed
speculation, PEA, vectorization, loop optimization, scheduling, barrier
optimization.

Status in this milestone: API complete (`include/vortex/j3/`), pipeline staged on
the roadmap in three waves (scalar core -> inlining/CIOG -> loops/vector/peephole).
