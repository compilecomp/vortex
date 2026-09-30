# Waiver: CEP&CC adoption window (M3 -> M4)

- Recorded: 2026-09-26
- Expires: 2026-12-31 (hard end of the M4 milestone — non-extendable
  without a new waiver)
- Owner: vortex maintainers (compilecomp/vortex)
- Waives: all findings reported by `tools/lint/cep_lint.sh` at the pinned
  tool commit `02dabbb7cd6b2087cc9e44121e8a97deef544048`, baseline
  recorded in `.cep/baseline.md` (2808 findings after the one-pass-per-
  file restructure and the M3 review fixes: 0 severity-0, 993 severity-1,
  1815 severity-2 across 125 files; pre-split reference 2678 across 108).
- Basis: CEP&CC 0.1 section 34.2 (severity-1 is "exterminate unless
  waived"); the migration is mechanical but annotation-heavy and must
  carry real per-function cost answers (Law 1), which forbids
  bulk-generated blocks.
- Termination condition: severity-1 count reaches zero and
  `CEP_LINT_ENFORCE=1` is set in CI; SEV2 backlog continues without a
  waiver because SEV2 does not gate.
- Progress accounting: each M4 batch updates `.cep/baseline.md` with the
  new counts; a batch that does not reduce the count does not count as
  progress (Rule 121 spirit: measured, not asserted).
- Amended 2026-09-30 (M4 closure): batch 1 (the J4 tree + its test/bench
  files) shipped CEP&CC-complete and severity-1 clean; batches 2+ (the
  remaining trees) continue per the remediation plan in
  `.cep/baseline.md`. The expiration DATE above is the binding term —
  the milestone parenthetical is superseded (the migration outlives the
  M4 boundary and is tracked batch by batch in docs/roadmap.md).
