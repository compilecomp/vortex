# Guest Semantics — Reference Language "Mini" (Oracle Registration)

**Status:** Authoritative for Mini. (Rule 106: a registered semantic oracle
defines observable behavior. Rule 130: compatibility is tracked here.)

Mini is the reference language port in `src/frontends/reference/`: the
executable form of `docs/porting.md`. It exists to (a) demonstrate how any
guest language lowers into UGB and (b) serve as the differential-testing
oracle until an external language runtime is registered.

## Registered oracle

For Mini, the oracle is:

1. **This document** — the definition of observable behavior; and
2. **T0 executing canonical UGB** — the universal correctness fallback tier
   (Rule 11). T0 executing canonical (non-speculative) opcodes is the
   reference implementation; every other engine path must be
   observationally indistinguishable from it (Rule 18).

The golden outputs under `examples/` and the regression suite in
`tests/` encode this oracle. Divergence without a registered exception is a
correctness bug (P1).

## Language subset (supported)

- 64-bit signed integer literals; `true`/`false`/`null`.
- `let` bindings, assignment, `if`/`else` (incl. `else if` chains),
  `while`, functions with parameters, `return` (implicit `return null`).
- Operators: `+ - * / %` (integer), comparisons `== != < <= > >=`,
  short-circuit `&& ||`, unary `-` (negation: `0 - x`) and `!`
  (logical not, matching branch truthiness: falsy = smi 0, null,
  undefined, false).
- Calls: direct calls to Mini functions; `print(...)` builtin.
- No floats in the surface syntax (float opcodes exist in UGB and are
  exercised via the text assembler), no closures, no exceptions, no FFI.

## Numeric semantics (Rule 110 — exact, no exceptions)

| Operation | Semantics |
|---|---|
| Integer values | 63-bit signed Smi (payload `<< 1` in the low word). Range: `[-2^62, 2^62-1]`. |
| Integer overflow (`+ - *`, unary `-`) | **Defined trap**: a guest-visible runtime error naming the operation and "integer overflow". Values never wrap, never silently truncate, and compilation never invokes signed-overflow UB (overflow-checked builtins). |
| Integer division by zero | Defined trap: "division by zero". |
| `smi_min / -1` and `smi_min % -1`-adjacent cases | Defined trap: "integer overflow". |
| Float values | IEEE-754 binary64, boxed on the heap. NaN, infinities and signed zero are legitimate values that flow through every float operation. |
| Float division by zero | IEEE: `1.0/0.0 = +inf`, `-1.0/0.0 = -inf`, `0.0/0.0 = NaN`. **Never** a substitute value. |
| Float comparisons | IEEE: NaN compares false against everything (including itself). |
| Float → Int conversion | **Saturating** (Wasm `trunc_sat` style): NaN → `0`; values above the Smi range clamp to `smi_max`; below clamp to `smi_min`; otherwise truncate toward zero. Never UB. |
| Int → Float conversion | Widening to binary64 with round-to-nearest (values above 2^53 round; below that, exact). |
| Bitwise/shift forms | `& \| ^` and `>>` (arithmetic + logical) compute in int64; results stay in the Smi range. Shift count is masked to `count & 63` (negative counts included: `-1 & 63 = 63`). `<<` that leaves the Smi range is a **defined trap: "integer overflow"** — it never wraps. |
| Float constant materialization | Through heap boxing only. `materialize_constant` returns `undefined` (not a truncated integer) for boxed kinds — hosts must materialize via the heap. |

## Equality and identity

- `==`/`!=` on integers compare values; on references compare **identity**
  (object identity is the raw reference; the young generation is non-moving,
  so identity is stable — Rule 84).
- `null` is a distinct singleton; `Eq.Null` tests it.

## Observable effects (Rule 107)

For Mini, the observable effect set is exactly: program output via `print`,
the returned value, guest-visible runtime errors (including the messages
class defined above), and termination. Allocation, IC state, profile
counters, tier transitions and rewriting are **not** observable through Mini
programs.

## Known divergences

None. (Rule 130 — this register must be updated in the same PR as any
behavior change.)

## Unsupported features

Closures/upvalues, generators/suspension, guest exceptions, FFI, dynamic
imports, reflection. Frontends for guests requiring these must declare the
corresponding capabilities (Rule 3) and the engine rejects methods whose
capabilities it cannot execute.
