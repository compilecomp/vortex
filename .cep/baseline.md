# CEP&CC adoption baseline

Recorded: 2026-09-26 (milestone M3 push); re-baselined same day after the
one-pass-per-file restructure of the J2/J3 pipelines (src/j2/pass_*.cpp,
src/j3/pass_*.cpp) — the split itself added 17 translation units, so the
per-file header and function-block findings rose mechanically.
Tool: `cep_lint` at CEP-CC `02dabbb7cd6b2087cc9e44121e8a97deef544048`
Command: `tools/lint/cep_lint.sh` (scans `include/vortex src tests tools`)

## Result

```text
132 files scanned, 2911 issues
  severity-0 (EXTERMINATE):        0    (re-recorded 2026-09-30, M4
  severity-1 (EXTERMINATE-UNLESS): 1003  closure; final post-review count)
  severity-2 (REJECT-AND-REPAIR):  1908
  severity-3 (WARN-AND-EDUCATE):      0

(M3 record: 125 files, 2808 issues — 0 / 993 / 1815 / 0;
pre-split reference: 108 files, 2678 issues — 0 / 974 / 1704 / 0;
post-split pre-review: 2794 — 0 / 991 / 1803; the M3 review fixes added
the two Rule-121 regression tests and re-touched pass TUs)
```

## Reading of the baseline

- **Severity-0 is already zero.** The banned-token rule for hot code
  (`new`/`delete`/`throw`/`try`/`catch`/`virtual`/`std::function`/...) is
  clean because the CEM-26 Rev 1.1 discipline (PERF_CONTRACT blocks,
  compliance.sh checks 65-69) banned the same constructs before the CEP&CC
  adoption. This is the load-bearing confirmation that the prior standard
  and the backbone standard agree on the hot-code core.
- **Severity-1 (993) is dominated by the annotation migration**: every
  file needs a nine-field `CEP:` header block from line 1
  (CEP-LINT-FILE-HEADER) and every function of two or more statements
  needs a seven-field block (CEP-LINT-FUNC-BLOCK). With 125 files and the
  current function density this is a mechanical but large rewrite — it is
  NOT automation-safe, because each block must carry a real cost answer
  (Law 1: no hidden cost; Law 8: no stale documentation). Bulk-generating
  blocks would manufacture compliance, which the standard exists to
  prevent.
- **Severity-2 (1815)** is mostly magic numbers in the interpreter/JIT
  staging code (the CEM-26 magic-number ban was enforced per-tree for hot
  files only, not tests/tools), macro names without the `CEP_` prefix
  (`VORTEX_*`, dispatch macros), and snake_case naming drift.

## Remediation plan (tracked in docs/roadmap.md, M4)

1. Hot trees first (`src/vm`, `src/j1`, `src/j2`, `src/j3`, `src/ir`,
   `src/runtime`, matching headers): CEP-0 headers + function blocks,
   reusing the existing PERF_CONTRACT facts as the cost answers.
2. Then CEP-1/CEP-2 trees (infra, support, ugb, tests, tools).
3. SEV2 backlog: name the magic numbers, prefix or retire the macros,
   fix naming — incrementally per touched file, not as a separate sweep.
4. Flip `tools/lint/cep_lint.sh` to enforced (`CEP_LINT_ENFORCE=1`) in CI
   when the SEV1 count reaches zero; until then the waiver in
   `.cep/waivers/` covers the gap.

## Batch 1 (M4, 2026-09-28): the J4 tree ships CEP&CC-complete

The J4 milestone code adopts the standard at birth (the pattern the
remaining batches follow):

- `include/vortex/j4/{max_jit,persistent_ir,worker}.hpp`,
  `src/j4/{max_jit,persistent_ir,worker}.cpp`, `src/j4/engine.hpp`:
  nine-field file headers and seven-field function blocks with real cost
  answers (compile-time costs; the fixed-point termination cost model is
  stated on every function).
- `tests/test_j4.cpp`: the M4 evidence base annotated per test (each
  test body carries its contract, cost and evidence pointer).
- `tools/bench/bench_tiers.cpp`: the M4 measurement harness annotated
  (workload, methodology, refusal reporting).

Batch result: the j4 tree is severity-1 clean (remaining j4 findings are
severity-2 naming notes on the `j2`/`J4Worker` names — namespace and
constructor spellings the whole repo shares, tracked in the SEV2
backlog). The new M4 files (the 5 j4 sources/headers, test_j4.cpp,
bench_tiers.cpp) contribute ZERO severity-1; the tree total moved
993 -> 1004 because the M4 delta TOUCHED un-migrated trees (the M3-review
follow-ups), adding +11 un-annotated functions there — per-file SEV1
delta: tests/test_j3.cpp +7 (the golden/splice-stamp/publish tests),
src/vm/interpreter.cpp +2 (the speculation-gate plumbing), rbpd.hpp,
rbpd.cpp, interpreter.hpp, fast_jit.cpp, passes_j3.cpp,
pass_escape_summary.cpp +1 each — and full_jit.cpp -1. Those files
inherit the batch-by-batch migration plan of their trees. The
un-migrated trees (vm/j1/j2/j3/ir/runtime/infra/ugb/gc/codegen/deopt)
are unchanged and remain waived.

Batch 1 closure amendment (2026-09-30, M4 review): the deopt-frame ABI
capacity test was split into its two observable contracts (record side:
the chain builds TO kMaxDeoptFrames; refusal side: the guard beyond the
capacity never enters the graph), both fully annotated, and the
BlacklistedTable test helper was restructured into an annotated factory
(an in-class constructor sits between the struct's CEP block and its
first member, which defeats the linter's function-block association).
The M4 review also added the cyclic-call-graph termination test (the
roadmap DoD's cycle-guard claim) and fixed a real M1-era defect it
exposed: helper_invoke_token resolved callees positionally
(method_table[token-1]) although the token pool is first-reference
ordered — forward-referenced callees (mutual recursion) silently called
the WRONG method; the fix resolves by name like T0 (backward-reference
shapes are bit-identical). Final closure count: 1003 SEV1 across 132
files — the batch-1 scope is still severity-1 clean, net -1 versus the
2026-09-28 record (SEV2 1907 -> 1908: one magic-number note in the new
cycle test's warmup constant — SEV2 does not gate).

## Batch 2 (M5, 2026-10-16): the GC tree ships CEP&CC-complete

The ICGGC milestone adopts the standard at birth for its own tree and
drives the touched-file delta to zero on the legacy files it modified:

- `include/vortex/gc/{icggc,collector}.hpp`, `src/gc/{icggc,collector,
  stack_scan}.cpp`: nine-field file headers + seven-field function blocks
  with real cost answers (the barrier legs' steady-state cycles, the pause
  bounds, the slice budgets).
- `tests/test_gc.cpp`: the M5 evidence base annotated per test.
- The M5-added functions in touched legacy files carry blocks:
  `gc_resolve` (object_model.hpp), `for_each_root_slot` (interpreter.hpp),
  `for_each_entry` (ffi.hpp), `resolve_brooks`/`store_frame_anchor`
  (stencil_corpus.cpp), `helper_barrier_store` (baseline_jit.cpp), the
  Interpreter ctor (interpreter.cpp).

Batch result: the gc tree + tests/test_gc.cpp are severity-1 clean (the
linter reports 0 SEV1 findings for every path under `/gc/` and
`tests/test_gc.cpp`). Per-file severity-1 deltas on the touched legacy
files (src/j2/fast_jit.cpp 84->84, src/j1/stencil_corpus.cpp 66->66,
src/vm/interpreter.cpp 25->25, src/j1/baseline_jit.cpp 21->21,
include/vortex/runtime/object_model.hpp 7->7, src/infra/ffi.cpp 6->6,
include/vortex/vm/interpreter.hpp 3->3, include/vortex/j1/
baseline_jit.hpp 2->2, include/vortex/infra/ffi.hpp 1->1): ZERO — the
M5-added functions were annotated, the pre-existing findings stay under
the waiver for their batches. The remaining severity-2 findings in the
new gc/test files are naming notes on the suite-wide `VORTEX_TEST`
macro and the fixture knob names — the same shape batch 1 recorded for
the j4 tree.

Re-recorded totals (2026-10-16, M5 closure):

```text
136 files scanned, 2968 issues
  severity-0 (EXTERMINATE):        0
  severity-1 (EXTERMINATE-UNLESS):  991  (M4: 1003 -> 991; -6 from the
                                        icggc.cpp migration + the GC-tree
                                        batch netting the new code to 0)
  severity-2 (REJECT-AND-REPAIR):  1977  (M4: 1908; +69 from the new gc/
                                        test code's naming notes, tracked
                                        for the SEV2 backlog)
  severity-3 (WARN-AND-EDUCATE):      0
```

## Batch 2 amendment (2026-10-16, post-review closure)

The 14b review round's fixes (stale-mark clearing, weak-handle upkeep, the
JIT TLAB handoff protocol, the regalloc safepoint treatment for the SATB
diversion stores) touched the same trees; the counts re-recorded at
closure:

```text
136 files scanned, 2975 issues
  severity-0 (EXTERMINATE):        0
  severity-1 (EXTERMINATE-UNLESS):  991
  severity-2 (REJECT-AND-REPAIR):  1984
  severity-3 (WARN-AND-EDUCATE):      0

(An interim 2979/0/992/1987 record was taken mid-fix-round; the 14d
verification pass's final counts are the ones above. The gc tree +
tests/test_gc.cpp: zero SEV1 findings at every record.)
```

The gc tree + tests/test_gc.cpp remain severity-1 CLEAN (0 findings). The
per-file SEV1 deltas on the touched legacy files stay ZERO against
dab39d5: fast_jit.cpp 84, stencil_corpus.cpp 66, interpreter.cpp 25,
baseline_jit.cpp 21, object_model.hpp 7, ffi.cpp 6, interpreter.hpp 3,
baseline_jit.hpp 2, ffi.hpp 1, context.hpp 2.
