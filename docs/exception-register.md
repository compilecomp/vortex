# Exception Register — UGB/VORTEX Compiler Laws

Rule 132: no rule may be silently bypassed. Every exception carries a rule id,
reason, owner, risk assessment, mitigation, telemetry and expiry date. Expired
exceptions become release blockers.

| ID | Rule | Reason | Owner | Risk | Mitigation | Telemetry | Expiry |
|---|---|---|---|---|---|---|---|
| EXC-001 | 29 | With `-fno-rtti` builds and no exception unwinding on the hot paths, an out-of-memory inside a std container growth path escalates to process termination rather than a guest-visible memory error. | compiler team | P2 — an OOM abort is confined to genuine heap exhaustion, not a compiler bug path | TLAB slow paths return `Result` errors for guest allocations (Rule 82 compliant); only host-side std containers can terminate; hosts can pre-size frames via config | allocation stats + slow-path counters exposed via `vx stats` | M2 (threading/heap handshake milestone) |
| EXC-002 | 11 (fallback completeness) / 16 (non-GNU toolchains) | The T0 computed-goto dispatch requires the GNU label-address extension; toolchains without it cannot execute the M0 dispatch loop, and the switch fallback is intentionally absent rather than shipping an untested second dispatch implementation (a second implementation would itself violate Rule 18's single-semantics contract until differentially tested). | compiler team | P1 on non-GNU/Clang compilers: engine refuses to start (safe rejection, Rule 29) instead of executing | GCC/Clang are the only supported toolchains in CI and README; the refusal is an explicit startup error, never silent misexecution | startup diagnostic names ADR-002 + this register | M2 (when the switch dispatch gets its own differential suite) |
| EXC-003 | 119 (differential testing) | Cross-tier differential testing (T0 vs J1–J4 vs oracle) cannot run before the tiers exist; M0 differentials are T0-vs-golden-outputs for the reference frontend. | testing team | P2 — single-engine risk is bounded by T0 being the spec's correctness fallback itself | golden outputs for `examples/*.mini`; numeric edge-case regression suite (Rule 121) | CI test counters | M1 (first J-tier landing) |

## Expired

(none)

## Process

1. File an entry above before merging any change that bypasses a rule.
2. Include rule ID, reason, owner, risk, mitigation, telemetry, expiry.
3. Waivers auto-expire on their expiry milestone; an expired waiver blocks release.
4. Silent bypass is itself a violation (lint review checklist, Rule 132).
