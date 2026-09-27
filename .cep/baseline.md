# CEP&CC adoption baseline

Recorded: 2026-09-26 (milestone M3 push); re-baselined same day after the
one-pass-per-file restructure of the J2/J3 pipelines (src/j2/pass_*.cpp,
src/j3/pass_*.cpp) — the split itself added 17 translation units, so the
per-file header and function-block findings rose mechanically.
Tool: `cep_lint` at CEP-CC `02dabbb7cd6b2087cc9e44121e8a97deef544048`
Command: `tools/lint/cep_lint.sh` (scans `include/vortex src tests tools`)

## Result

```text
125 files scanned, 2808 issues
  severity-0 (EXTERMINATE):        0
  severity-1 (EXTERMINATE-UNLESS): 993
  severity-2 (REJECT-AND-REPAIR):  1815
  severity-3 (WARN-AND-EDUCATE):      0

(pre-split reference: 108 files, 2678 issues — 0 / 974 / 1704 / 0;
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
