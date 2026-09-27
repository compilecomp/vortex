#!/bin/sh
# CEP&CC adoption shim: runs the backbone standard's cep_lint tool
# (github.com/axiomzero0/CEP-CC) against this tree.
#
# CEP:WHAT: Fetch/build cep_lint at a pinned commit and lint include/src/tests/tools.
# CEP:WHY: The backbone standard (CEP&CC 0.1) is enforced by its own tool,
#          not reimplemented here; policy stays single-sourced in the
#          standard repository (Law 8: no stale documentation).
# CEP:CLASS: CEP-2
# CEP:STATUS: complete
# CEP:FAILURE: Exits non-zero on clone/build failure, or with
#              CEP_LINT_ENFORCE=1 when the lint reports findings.
# CEP:ASSUMES: git and a C++26 compiler are available; the standard repo
#              is reachable at the pinned commit (or CEPCC_DIR points at
#              an existing checkout).
# CEP:COST: Offline after the first clone; lint itself is single-threaded
#           and linear in the scanned tree (~100 files < 1 s).
# CEP:EVIDENCE: .cep/baseline.md (adoption baseline) and .cep/waivers/.
#
# Enforcement status: REPORT-ONLY until the M4 CEP&CC migration completes
# (baseline: 2794 findings across 125 files, .cep/baseline.md). Set
# CEP_LINT_ENFORCE=1 to propagate the linter verdict — CI flips this after
# the SEV1 migration. The dated waiver for this window lives in
# .cep/waivers/ (CEP&CC 34: waivers without expiration are banned).

set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CEPCC_URL="https://github.com/axiomzero0/CEP-CC.git"
CEPCC_PIN="02dabbb7cd6b2087cc9e44121e8a97deef544048"
CEPCC_DIR="${CEPCC_DIR:-$(dirname "$ROOT")/cepcc}"

if [ ! -d "$CEPCC_DIR/.git" ]; then
    echo "cep_lint.sh: cloning CEP-CC into $CEPCC_DIR" >&2
    mkdir -p "$(dirname "$CEPCC_DIR")"
    git init -q "$CEPCC_DIR"
    git -C "$CEPCC_DIR" remote add origin "$CEPCC_URL"
    git -C "$CEPCC_DIR" fetch -q --depth 1 origin "$CEPCC_PIN"
    git -C "$CEPCC_DIR" checkout -q FETCH_HEAD
fi

ACTUAL="$(git -C "$CEPCC_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
if [ "$ACTUAL" != "$CEPCC_PIN" ]; then
    echo "cep_lint.sh: CEPCC_DIR is at $ACTUAL, pinned $CEPCC_PIN" >&2
    echo "cep_lint.sh: refusing to lint with an unpinned standard (set CEPCC_DIR to override)" >&2
    exit 2
fi

BIN="$CEPCC_DIR/tools/cep_lint/build/cep_lint"
NEWEST_SRC="$(find "$CEPCC_DIR/tools/cep_lint/src" "$CEPCC_DIR/.cep" \
                  -name '*.cpp' -o -name '*.hpp' -o -name '*.json' \
                  2>/dev/null | xargs ls -t 2>/dev/null | head -1)"
if [ ! -x "$BIN" ] || [ "$NEWEST_SRC" -nt "$BIN" ]; then
    echo "cep_lint.sh: building cep_lint" >&2
    MODE=release "$CEPCC_DIR/tools/cep_lint/build.sh" >&2
fi

cd "$CEPCC_DIR"
set +e
"$BIN" -c "$CEPCC_DIR/.cep/cep_lint.json" \
    "$ROOT/include/vortex" "$ROOT/src" "$ROOT/tests" "$ROOT/tools"
STATUS=$?
set -e

echo "cep_lint.sh: linter exit $STATUS (0 clean, 1 findings, 2 boot error)"
if [ "$STATUS" -ne 0 ] && [ "${CEP_LINT_ENFORCE:-0}" != "1" ]; then
    echo "cep_lint.sh: report-only mode (waiver window until the M4 migration; see .cep/waivers/)"
    exit 0
fi
exit "$STATUS"
