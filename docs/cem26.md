# CEM-26 Compliance — VORTEX (Rev 1.1)

This document maps the **CEM-26 Revision 1.1** standard (Cycle-Exact
Maintainable C++26 Code Standard for Compiler/JIT Infrastructure) onto the
VORTEX codebase. It defines the scope of "important parts", the annotation
conventions, the cost-block conventions, the live **PERF_PERMIT register**,
and the validation plan that turns `ACTUAL: PENDING` observations into
measured numbers.

Normative wording follows the standard: `MUST`/`REQUIRED` are blocking,
`MUST NOT`/`FORBIDDEN` are absolute, `SHOULD` requires a written
justification (`PERF_NOTE`), `MAY` requires documented cost.

---

## 1. Scope: the important parts

CEM-26 applies in full to the **hot trees** — the code executed per
instruction, per IR node, or per allocation:

| Tree | Contents | Classification |
|------|----------|----------------|
| `src/vm`, `include/vortex/vm` | T0 dispatch loop, decoder, IC protocol, frame pool | `@hot` / `@warm` |
| `src/gc`, `include/vortex/gc` | TLAB bump allocation, card table, object headers | `@hot` / `@warm` |
| `src/ir`, `include/vortex/ir` | SoN graph storage, DCE worklists | `@hot` per pass |
| `include/vortex/support` | `TaggedValue`, `FlatHashMap`, `SmallVector`, `SparseSet` | `@hot` members |
| `include/vortex/ugb/encoding.hpp` | wire-layout constants (single source of truth) | `constexpr` only |

`@warm` covers per-call/per-module-load frequency (name-resolution caches,
IC state transitions, heap refill). `@cold` covers setup, diagnostics,
serialization, and the text assembler/verifier/disassembler tooling — those
MAY use standard C++ conveniences but MUST NOT be reachable from hot code
without an explicit boundary comment (all such boundaries are marked
`BOUNDARY` at the call site).

## 2. Annotation conventions

- `// @hot` / `// @warm` / `// @cold` precede the declaration or definition.
- A `PERF_CONTRACT:` block (BUDGET / READS / WRITES / BRANCHES / CACHE) is
  the **author's promise** and lives at the definition. When the definition
  is in a `.cpp` and the declaration in a header, the header carries a
  one-line `PERF_CONTRACT: see <file>` pointer instead of duplicating the
  numbers — duplicated budgets drift, and a drifted contract is worse than
  a referenced one.
- A `PERF_OBSERVATION:` block (TARGET / VALIDATED / ACTUAL /
  LAST_VALIDATED) records the machine's reality.
- `PERF_NOTE` documents a known theoretical cost that profiling shows is
  not a regression (CEM-26 section 5). `PERF_PERMIT` documents a granted
  exception (CEM-26 section 17) and MUST be registered in section 5 below —
  this is enforced mechanically by `tools/lint/compliance.sh`.

## 3. Cost Trinity state (M0)

Per CEM-26 section 6, hot functions document the **CEM Source Cost** today.
The **Validated Assembly Cost** and **Target µarch Cost** layers are tracked
as follows:

- Contracts in the code are the cost of record. Where a contract could be
  verified against generated assembly today (e.g. `klass_token_key`: one
  shift + one or, zero memory ops), `ACTUAL` states that observation and
  its toolchain.
- Contracts that require a microbenchmark harness carry
  `ACTUAL: PENDING microbench (M0)` and are tracked in the validation plan
  below. A `PENDING` observation is an explicit debt, not a waiver: the
  BUDGET line remains the review baseline, and any change that visibly
  exceeds it in generated assembly fails review even without the harness.
- **Validation plan (M1):** a `bench/` target with per-contract
  microbenchmarks (dispatch loop, decode, IC lookup, TLAB alloc, card mark,
  FlatHashMap probe), pinned to the target list in each contract
  (Zen 5 / ARM Neoverse V2 class cores), CI-archived so
  `LAST_VALIDATED` refreshes mechanically. Each benchmark asserts the
  BUDGET, and a >10% regression fails CI (CEM-26 section 5).

## 4. Layout audits (CEM-26 section 9)

Every hot struct states its size and alignment next to its definition:

| Struct | Size | Rationale |
|--------|------|-----------|
| `TaggedValue` | 8 B | the register-file word |
| `Decoded` (T0) | 24 B | register-carried across dispatch jumps; reordered so `next_pc` leads and no tail padding exists |
| `ArithResult` | 16 B | register-returned from the arithmetic slow paths |
| `ProfileSlot` | 32 B | dense per-instruction array; AoS chosen over SoA because the record calls fire together |
| `IcEntry` | 8 B | two u32 words |
| `IcSlot` | 48 B | reordered (scalars first) to eliminate tail padding; poly scan stays line-local |
| `FlatHashMap::Slot` | ≤ 32 B | probe locality granule |

Pointer chasing is absent from all hot paths: the register file is a pinned
contiguous buffer, IC/profile slots are dense per-instruction vectors, and
the only hash structure on a hot path (`FlatHashMap`) is open addressing
with power-of-two capacity.

## 5. PERF_PERMIT register (live)

Exceptions granted under CEM-26 section 17. Each entry must remain
justified; the reviewer may revoke any permit at any time.

### PERF-001 — computed-goto threaded dispatch (`execute`, src/vm/interpreter.cpp)
- **REASON:** section 10 bans indirect *calls*; dispatch is an indirect
  *jump* within one function — the data-oriented dispatch the standard's
  vocabulary prescribes (ADR-002).
- **COST:** one BTB-predicted indirect jump per instruction; no stack
  traffic, no call/ret pair.
- **OWNER:** @vortex/rt

### PERF-002 — `CALL_BUILTIN` host-function pointer (src/vm/interpreter.cpp)
- **REASON:** the builtin ABI is the host-extension boundary (token-keyed,
  Rule 5). The call is the guest-visible operation itself, not per-item
  overhead inside a loop.
- **COST:** one indirect call per `CALL_BUILTIN` execution; target is stable
  per site after mono resolution, BTB-predicted.
- **OWNER:** @vortex/rt

### PERF-003 — `SAFEPOINT_POLL` hook pointer (src/vm/interpreter.cpp)
- **REASON:** the suspension handshake must be replaceable without
  recompiling the engine (Rule 81). Converts to a data-oriented atomic poll
  word when the M2 threading infra lands.
- **COST:** one null test (common case, no hook installed) + rare indirect
  call.
- **OWNER:** @vortex/rt — expiry: M2 threading milestone

### PERF-004 — per-instruction bigram probe (`record_bigram`, src/vm/interpreter.cpp)
- **REASON:** bigram heat is the J1 superstencil promotion input
  (docs/tier-j1.md section 3); sampling would lose the distribution tail
  that drives fusion decisions.
- **COST:** one splitmix64 hash + ~1 open-addressing probe per instruction,
  included in the `execute` master budget.
- **OWNER:** @vortex/rt

### PERF-005 — mprotect pair per patch session (`PatchArena`, src/infra/patch_arena.cpp; `LdptManager::patch_session`, src/runtime/ldpt.cpp)
- **REASON:** the W^X law (infra/security.hpp) makes every code write a
  two-flip RX→RW→RX session. LDPT patch sessions batch all writes of one
  escalation (hole + OOL stub, ≤ 272 bytes) behind one flip pair under the
  M:N safe-point protocol (docs/ldpt.md section 2).
- **COST:** ≤ 20 µs per session (2 syscalls + handshake + memcpy),
  amortized to zero against the patched steady state — reached once per
  (site, type) pair.
- **OWNER:** @vortex/rt — expiry: M8 code-cache compaction (arena batches)

### PERF-006 — allocation on the compile/bind path (`make_j1_bindings`,
`BaselineJit::compile`, src/j1/baseline_jit.cpp)
- **REASON:** J1 compilation is @warm per-method work (tier-up event); the
  constant-pool materialization and metadata buffers allocate there.
  Steady-state guest execution allocates only through the corpus's inline
  TLAB bump paths (no C++ allocation).
- **COST:** O(constants + classes) allocations per method compile, inside
  the J1 latency budget asserted by tests (10 ms CI-safe bound).
- **OWNER:** @vortex/rt

### PERF-007 — LDPT resolver cold path (`resolve_miss`, src/runtime/ldpt.cpp)
- **REASON:** the resolver runs once per (site, type) pair: codegen of the
  marshalling stub + one patch session + dispatch. It is the machinery that
  removes the per-call IC/indirect cost from every subsequent execution
  (docs/ldpt.md section 5 contract table).
- **COST:** ~2-5 µs first miss per type; amortized to zero in steady state.
  OOM during escalation terminates deliberately (allocator failure inside a
  patch protocol is not recoverable in-place).
- **OWNER:** @vortex/rt

## 6. Mechanical enforcement

`tools/lint/compliance.sh` (CI job `compliance`) enforces the mechanically
checkable subset in the hot trees:

- no `std::memory_order_seq_cst` (section 14 — orders must be stated
  explicitly as acquire/release),
- no `std::mutex` / `std::lock_guard` / `std::unique_lock` /
  `std::shared_mutex` (sections 7 and 14),
- every file containing `// @hot` carries a `PERF_CONTRACT` block or a
  pointer to it (sections 3 and 4),
- every `PERF_PERMIT` site in code carries a `PERF-00N` ID and every ID
  exists in the section 5 register of this document (section 17). This is
  an ID-presence check: the register entry must still be reviewed for
  REASON/COST/OWNER completeness — the lint catches unregistered permits,
  not weak justifications.

Together with the existing Compiler-Laws checks (no `throw`, no RTTI, no
node-based containers, no `shared_ptr`/`std::function` in hot trees), the
Immediate Rejection List of CEM-26 section 17 is covered item by item:
magic numbers are removed at review (and banned in new code by the
encoding/IC/GC constant registries), heap allocation in hot code is
confined to three documented growth paths — the two high-water paths in
`execute` (frame pool, runtime tables) and the bounded bigram-table rehash
(`record_bigram`; key space capped by the opcode alphabet, stops after
warmup) — and hidden copies/temporaries are banned by the existing
no-`std::string`-on-hot-path rule.

## 7. Review protocol

Reviews of hot-tree changes answer the 13 questions of CEM-26 section 17
against the contract blocks: instructions (BUDGET), cache lines (CACHE),
branches (BRANCHES), dependency chains (BUDGET notes), hidden state (the
`transitions_` ring and `stats_` block are the only hidden mutable state,
both single-mutator and L1-resident), inlining (hot functions are
inline-sized and marked), contention (single-mutator M0 contract; M2
handshake owns cross-thread publication), empty/huge inputs (bounds tests at
boundaries, `max_array_length`/`max_call_depth` knobs), alignment (heap
16-byte invariant, word-aligned tagged values), compiler deferral (no LTO
reliance — contracts hold at `-O2`), magic numbers (constant registries),
layout/access-pattern match (section 4 audits), and maintainer
understandability (this document).
