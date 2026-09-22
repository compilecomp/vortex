# XLEA — Cross-Language Escape Analysis

Polyglot interop drowns in short-lived **wrapper allocations**: every
cross-language call pays an allocation (Language A wraps its arguments
into a tuple/box), N member dispatches (Language B reads them back), and
GC pressure — and the wrapper is garbage immediately after. This
dominates interop cost more than call overhead itself.

XLEA eliminates it. V8 and GraalVM do escape analysis *within* one
language. **Cross-language speculative EA with RBPD fallback is the
differentiator**: when the wrapper doesn't escape the merged program, the
allocation and the interop dispatches vanish entirely.

Status in this milestone: **spec complete** — the mechanism, the
escape-summary data structure and the guard-emission rules are defined
below and reflected in `include/vortex/ir/escape_summary.hpp`.
Implementation lands with the J3/J4 inlining pipeline (docs/roadmap.md).

---

## 1. Why it is hard cross-language

Classic EA works within one compilation unit. Here:

- the wrapper is created in Language A's unit,
- consumed in Language B's unit,
- compiled separately, possibly at different tiers.

**The Vortex advantage: J4 has whole-program visibility via CIOG.** When
the devirt graph inlines Language B into Language A's unit, EA runs over
the *merged* SoN graph and sees what no per-language compiler can.

## 2. The four-phase mechanism

**Phase 1 — Escape analysis on the merged graph.**
After cross-language inlining, run EA over the combined SoN:

- mark allocation nodes,
- propagate escape status: an allocation **escapes** if it is stored to
  the heap, passed to a non-inlined call, returned, or GC-rooted,
- non-escaping allocations are marked for scalar replacement.

**Phase 2 — Scalar replacement.**
Replace the wrapper object with its fields as SSA values. Field
reads/writes become direct value uses; the allocation is deleted.

**Phase 3 — Cross-language coordination (the payoff).**
Language A's "store field" becomes a direct SSA definition; Language B's
`READ_MEMBER` becomes a direct SSA use. **The interop message dispatch is
eliminated entirely.** The cross-language call collapses into direct
value passing:

```text
WITHOUT XLEA:
  alloc wrapper → store a → store b → POLY_EXECUTE
  → READ_MEMBER[0] → READ_MEMBER[1] → add → GC later

WITH XLEA (after inlining):
  add(a, b)        ← wrapper and dispatches GONE
```

**Phase 4 — Guard emission.**
J4 emits guards that the wrapper (see section 5):

- is never stored past the merged region's lifetime,
- is never reflectively enumerated (`MEMBERS`),
- is never GC-rooted.

A failed guard triggers an RBPD deopt of exactly that region — the rest
of the compiled method keeps running (docs/deopt-rbpd.md).

## 3. Speculative XLEA (the bananas extension)

Even when J4 **cannot prove** non-escape (Language B *might* store the
wrapper), if profiling shows it *never does*, J4 **speculatively
scalar-replaces with a guard**. RBPD provides the safe fallback. This
turns aggressive EA from "risky" into "free to attempt": the worst case
is one deopt and a recompile without the speculation, never a wrong
result.

Speculation inputs are the existing profile machinery: the per-site
IC/profile slots (docs/tier-t0.md section 6) already record which
messages fired on a wrapper; "no STORE_MEMBER/MEMBERS/GC-root observed"
over the hot threshold is the speculation license.

## 4. Cross-tier escape summaries

If Language B's callee is at J3 but the caller compiles at J4, J4 must
not re-analyze. Every optimizing-tier compile publishes an **escape
summary** per function; callers consume summaries without re-analysis.
This makes XLEA compositional across tiers and across languages.

### 4.1 The data structure

```cpp
// include/vortex/ir/escape_summary.hpp
struct EscapeSummary {
    uint32_t param_count;
    // Per parameter: does the argument (or anything reachable from it)
    // outlive the call?
    enum ParamEscape : uint8_t {
        NoEscape,        // not stored, not returned, not globalized
        ArgEscape,       // stored somewhere the callee cannot see
        Unknown,         // summary unavailable for this parameter
    };
    ParamEscape params[];      // indexed by parameter position
    bool returns_heap_allocation; // the return value is freshly allocated
    bool captures_global;         // stores into globals/statics
    uint64_t graph_hash;          // SoN graph hash this summary describes
};
```

Laws:

- **Soundness**: a summary may only claim `NoEscape` when the analysis
  that produced it saw the **whole** callee body (no FFNI — foreign
  function — boundary crossed inside; any un-inlinable call forces
  `ArgEscape` or `Unknown`).
- **Identity**: `graph_hash` binds the summary to the exact compiled
  graph; a recompile invalidates published summaries (the dependency
  graph routes the invalidation, the same channel LDPT sites use).
- **Monotonicity**: a summary may be *weakened* (NoEscape → ArgEscape)
  on invalidation, never strengthened without a recompile.
- Storage: per (method id, graph hash) in the persistent IR store
  (docs/roadmap.md M4); keyed by ids, never strings (Rule 5).

### 4.2 Consumption

- A caller whose argument is a wrapper allocation consults the callee's
  summary: `NoEscape` licenses Phase 1–3 replacement; `ArgEscape` forces
  the real allocation; `Unknown` falls back to speculative XLEA
  (section 3) if the profile licenses it, otherwise the real allocation.
- Summaries compose: inline chains take the conjunction — one
  `ArgEscape` anywhere along the chain escapes.

## 5. Guard-emission rules

Every scalar replacement is guarded. The guard set is exactly the set of
escape channels the analysis assumed closed, plus the message set that
would have bypassed them:

| # | Guard | Fires when | RBPD action |
| :- | :--- | :--- | :--- |
| G1 | **Lifetime** | the wrapper's identity is stored anywhere reachable past the merged region (heap store, global store, returned by an un-inlined call) | deopt the region; recompile without replacement |
| G2 | **Reflectability** | `MEMBERS` / `HAS_MEMBERS` / `IS_MEMBER_READABLE` / `IS_MEMBER_WRITABLE` executes on the wrapper | deopt the region; materialize the wrapper and resume it through the vtable |
| G3 | **GC-rooting** | a safepoint map would root the wrapper (its pseudo-register range reaches a call whose GC map includes it) | deopt the region; materialize before the next safepoint |
| G4 | **Identity comparison** | `EQ_REF`/`NE_REF` compares the wrapper against anything | deopt the region; scalar-replaced objects have no identity |
| G5 | **Exceptions** | `THROW`/`IS_EXCEPTION` targets the wrapper, or an inlined callee can throw while the wrapper is live in a handler's visibility | deopt the region; materialize into the handler path |

Rules:

- Guards are emitted **at the operation that would violate the
  assumption**, not at the allocation: G1 at each candidate store, G2 at
  each reflective message site, G3 as safepoint-map constraints (the
  wrapper's vregs are simply excluded from root maps — a *negative*
  guard that costs nothing), G4 at comparison sites, G5 at handler
  boundaries.
- Materialization on deopt is the RBPD continuation's job: the
  region's deopt record carries the (field → SSA value) mapping, and the
  interpreter/next tier rebuilds a real wrapper with those values — the
  same materialization machinery deopt already uses for scalar-replaced
  non-cross-language objects, extended with the wrapper's klass token.
- G2/G4 are *profile-gated speculative* guards: when the profile shows
  the message never fires, the guard still exists but is a single
  never-taken branch on the message id — the RBPD cost model already
  prices never-taken guards at zero (docs/deopt-rbpd.md).
- Guard failure counters feed the same speculation demotion channel the
  typed-arithmetic rewrites use (Rule 43): chronic G2 hits demote the
  site to real-allocation code permanently.

## 6. Cost model (CEM-26 view)

- The merged-graph EA is @warm per compile (J3/J4 compile-time work,
  allocation confined to the compile path — PERF-006's pattern).
- The steady-state win is structural: N dispatches + 1 allocation + GC
  pressure → zero. The guards are branchless or never-taken branches on
  the executed path.
- G3 costs nothing at runtime (root-map exclusion is a metadata
  decision), which is why GC-rooting is an *assumption* rather than a
  runtime check.

## 7. Next in the queue

**Redundant Barrier Elimination** — the ICGGC write barrier is a
dataflow-redundant store for wrapper-heavy interop (the same card marked
N times, barriers after stores already provably dirty); a dedicated
dataflow pass over the merged SoN kills the tax. Then Cross-Language
Partial Evaluation.
