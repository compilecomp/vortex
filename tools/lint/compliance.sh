#!/bin/sh
# Compliance lint — mechanical enforcement of hot-path structural laws.
# (Compiler Laws Rules 48/50/65/68; see docs/compliance_matrix.md)
#
# Usage: tools/lint/compliance.sh <repo-root>
set -u

ROOT="${1:-.}"
fail=0

check() {
    # check <rule> <description> <grep-regex> <paths...>
    rule="$1"; desc="$2"; regex="$3"; shift 3
    matches=$(grep -rnE -- "$regex" "$@" 2>/dev/null | grep -v 'compliance.sh' | head -20)
    if [ -n "$matches" ]; then
        echo "VIOLATION Rule $rule ($desc):"
        echo "$matches"
        fail=1
    else
        echo "ok       Rule $rule: $desc"
    fi
}

# Rule 65: no native exceptions in the core library (all of src/ + include/).
# Guest exceptions are runtime values (Rule 66); error paths use Result.
check 65 "no throw in core library" '\bthrow ' \
    "$ROOT/src" "$ROOT/include"

# Rule 50: node-based hash containers are forbidden on hot paths
# (vm / ir / gc / runtime trees). Cold tools (frontends, ugb tooling, infra)
# may use them for one-shot setup.
check 50 "no unordered_map/map in hot trees" 'std::unordered_(map|set)|std::map<|std::set<|std::multiset<' \
    "$ROOT/src/vm" "$ROOT/src/ir" "$ROOT/src/gc" "$ROOT/src/runtime" \
    "$ROOT/include/vortex/vm" "$ROOT/include/vortex/ir" \
    "$ROOT/include/vortex/gc" "$ROOT/include/vortex/runtime"

# Rule 48: IR edges are NodeId indices; raw pointer edges are forbidden.
check 48 "no raw-pointer IR edges" 'std::vector<Node\*>|Node\* [a-z_]+_;|data_inputs\.push_back\(&' \
    "$ROOT/include/vortex/ir" "$ROOT/src/ir"

# Rule 68: no RTTI constructs in compiler/runtime code (kind enums instead).
check 68 "no RTTI (dynamic_cast/typeid)" '\bdynamic_cast<|\btypeid\(' \
    "$ROOT/src" "$ROOT/include"

# Rule 69: no shared_ptr / std::function in hot IR code.
check 69 "no shared_ptr/function in hot trees" 'std::shared_ptr|std::function' \
    "$ROOT/include/vortex/ir" "$ROOT/src/ir" "$ROOT/include/vortex/vm" "$ROOT/src/vm"

# ---- CEM-26 Rev 1.1 (Cycle-Exact Maintainable C++26) -------------------------
# Mechanical subset of the standard; the backbone standard is now CEP&CC 0.1
# (github.com/axiomzero0/CEP-CC, enforced by tools/lint/cep_lint.sh) and the
# PERF_PERMIT register is embedded below (s17 check). Hot trees = vm, ir,
# gc, runtime.

cem_fail=0

# CEM-26 section 14: seq_cst is banned in hot code without a PERF_PERMIT.
# Hot code must state its memory orders explicitly (acquire/release).
cem_matches=$(grep -rnE 'std::memory_order_seq_cst|memory_order::seq_cst' \
    "$ROOT/src/vm" "$ROOT/src/ir" "$ROOT/src/gc" "$ROOT/src/runtime" \
    "$ROOT/include/vortex/vm" "$ROOT/include/vortex/ir" \
    "$ROOT/include/vortex/gc" "$ROOT/include/vortex/runtime" 2>/dev/null | head -20)
if [ -n "$cem_matches" ]; then
    echo "VIOLATION CEM-26 s14 (seq_cst in hot trees):"
    echo "$cem_matches"
    cem_fail=1
else
    echo "ok       CEM-26 s14: no seq_cst in hot trees"
fi

# CEM-26 section 7/14: no mutexes in hot trees (message passing / lock-free
# per the infrastructure docs).
cem_matches=$(grep -rnE 'std::mutex|std::lock_guard|std::unique_lock|std::shared_mutex' \
    "$ROOT/src/vm" "$ROOT/src/ir" "$ROOT/src/gc" "$ROOT/src/runtime" \
    "$ROOT/include/vortex/vm" "$ROOT/include/vortex/ir" \
    "$ROOT/include/vortex/gc" "$ROOT/include/vortex/runtime" 2>/dev/null | head -20)
if [ -n "$cem_matches" ]; then
    echo "VIOLATION CEM-26 s14 (mutex in hot trees):"
    echo "$cem_matches"
    cem_fail=1
else
    echo "ok       CEM-26 s7/14: no mutex in hot trees"
fi

# CEM-26 sections 3/4: every file that claims a hot function must carry at
# least one PERF_CONTRACT block (the @hot functions' cost block of record).
for f in $(grep -rl '// @hot' \
          "$ROOT/src/vm" "$ROOT/src/ir" "$ROOT/src/gc" "$ROOT/src/ugb" \
          "$ROOT/src/runtime" \
          "$ROOT/include/vortex/vm" "$ROOT/include/vortex/ir" \
          "$ROOT/include/vortex/gc" "$ROOT/include/vortex/support" \
          "$ROOT/include/vortex/runtime" \
          "$ROOT/include/vortex/ugb" 2>/dev/null); do
    if ! grep -q 'PERF_CONTRACT' "$f"; then
        echo "VIOLATION CEM-26 s4: $f marks hot code but has no PERF_CONTRACT block"
        cem_fail=1
    fi
done
if [ "$cem_fail" -eq 0 ]; then
    echo "ok       CEM-26 s3/4: all @hot files carry PERF_CONTRACT blocks"
fi

# CEM-26 sections 4/17: every PERF_PERMIT site must carry a registered ID
# (PERF-00N). The register lives HERE, where it is enforced (Law 8: no
# stale documentation): docs/cem26.md was removed upstream when the
# CEP&CC standard (github.com/axiomzero0/CEP-CC) became the backbone —
# the permit register is the one CEM-26 artifact the toolchain still
# consumes, so it is embedded below with its justifications. Adding a
# permit = adding its entry here AND a waiver note under .cep/waivers/
# (CEP&CC 34: waivers carry an owner and an expiration).
permit_register() {
    cat <<'EOF'
PERF-001: computed-goto threaded dispatch (execute, src/vm/interpreter.cpp)
PERF-002: CALL_BUILTIN host-function pointer (src/vm/interpreter.cpp)
PERF-003: SAFEPOINT_POLL hook pointer (src/vm/interpreter.cpp)
PERF-004: per-instruction bigram probe (record_bigram, src/vm/interpreter.cpp)
PERF-005: mprotect pair per patch session (PatchArena, src/infra/patch_arena.cpp; LdptManager::patch_session, src/runtime/ldpt.cpp)
PERF-006: allocation on the compile/bind path (make_j1_bindings, src/j1/baseline_jit.cpp)
PERF-007: LDPT resolver cold path (resolve_miss, src/runtime/ldpt.cpp)
EOF
}
permit_ids=$(grep -rhoE 'PERF-[0-9]+' "$ROOT/src" "$ROOT/include" 2>/dev/null | sort -u)
if [ -n "$permit_ids" ]; then
    unregistered=""
    for id in $permit_ids; do
        if ! permit_register | grep -q "^$id:"; then
            unregistered="$unregistered $id"
        fi
    done
    if [ -n "$unregistered" ]; then
        echo "VIOLATION CEM-26 s17: PERF_PERMIT ID(s) not registered in the embedded register:$unregistered"
        cem_fail=1
    else
        n=$(echo "$permit_ids" | wc -l | tr -d ' ')
        echo "ok       CEM-26 s17: all $n PERF_PERMIT ID(s) registered (embedded register)"
    fi
else
    echo "ok       CEM-26 s17: no PERF_PERMIT sites in code"
fi

if [ "$cem_fail" -ne 0 ]; then
    echo "compliance lint: FAILED"
    exit 1
fi

if [ "$fail" -ne 0 ]; then
    echo "compliance lint: FAILED"
    exit 1
fi
echo "compliance lint: PASSED"
exit 0
