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
