# ICGGC — Incremental Concurrent Generational GC

The Vortex collector is incremental, concurrent, generational, region-based in the
old generation, pause-bounded, compaction-capable, and JIT-integrated.

---

## 1. Heap shape

### Young generation
Bump-pointer allocation, TLAB allocation, short-lived objects, fast collection,
copied/evacuated on collection.

### Old generation
Region-based, remembered sets, concurrent marking, concurrent sweeping,
concurrent evacuation where possible.

### Humongous space
Objects larger than a region; separate allocation and collection rules.

---

## 2. Allocation fast path

```text
load TLAB top
add object size
compare with TLAB end
if ok:
    store new top
    initialize header
    return object
else:
    slow path
```

JIT optimizations on top: allocation folding, TLAB bump folding, allocation-site
classification, scalar replacement, stack allocation through escape analysis.

---

## 3. Write barriers

Write barriers track cross-generational references and concurrent-marking state.

Forms: card table barrier, remembered set barrier, SATB pre-write barrier,
compressed barrier, inline barrier, outlined barrier.

Barrier optimizations: elision via escape analysis, temporal batching, hoisting,
outlining, speculative removal, elimination via temporal locality.

### 3.1 SATB marking

Old-generation concurrent marking uses Snapshot-At-The-Beginning. Mutators log
overwritten references:

```text
old_value = *slot
log old_value
*slot = new_value
```

This preserves the tri-color invariant.

---

## 4. Read barriers

Read barriers support concurrent compaction. Forms: Brooks pointer read barrier,
forwarding pointer check, tagged pointer read barrier, hardware-assisted read
barrier.

Optimizations: elision when the object cannot move, hardware-assisted checks,
JIT-proven immobility, region pinning during critical sections.

---

## 5. Concurrent compaction

```text
1. Select victim regions.
2. Concurrently copy live objects.
3. Install forwarding pointers.
4. Update references.
5. Update remembered sets.
6. Reclaim old regions.
```

Requires read barriers, forwarding metadata, self-relocating object support, and
JIT read-barrier integration.

---

## 6. Incremental pacing

GC work is split into small slices. The pacer adjusts based on allocation rate,
live-set growth, heap occupancy, mutator throughput, and the pause budget. Goal:
keep pause time below target while avoiding OOM.

---

## 7. GC/JIT metadata interchange

GC uses JIT metadata: GC maps, register masks, deopt frame descriptors, compressed
oop metadata, object layout metadata, allocation-site metadata, barrier stub
metadata.

JIT uses GC metadata: heap base, card table base, TLAB pointers, region tables,
barrier mode flags, concurrent phase state.

Additional runtime features: incremental root scanning, stack watermarking,
register mask caching, GC map compression, deopt-aware root scanning, NUMA-aware
allocation, slab/size-class allocation, page-level eviction, lazy sweeping.

---

## 8. M5 — the ICGGC concurrent engine (this milestone)

M5 replaces the M0 stubs with the working engine. Everything below is the
binding contract for `include/vortex/gc/icggc.hpp`, `src/gc/*.cpp`, the mutator
barriers in T0/J1/J2-J4, and `tests/test_gc.cpp`.

### 8.1 Heap layout v2

One contiguous buffer, two spaces:

```text
[ young space | old space (regions) ]
  ^heap_base     ^young_end = old_base
```

* Young space: the M0 bump allocator, unchanged shape (TLAB chunks,
  16-byte object alignment). Evacuated on young collection.
* Old space: `kRegionBytes` regions (256 KiB; semantic domain — changing it
  is a GC-protocol event). Region states: `Free`, `Bump` (promotion target,
  bump-allocated), `Occupied` (full, awaiting mark/sweep), `CSet`
  (selected for evacuation), `HumongousStart`/`HumongousCont` (one object
  larger than a region, contiguous region run, never evacuated — separate
  allocation and collection rules, section 1).
* `Heap::collect_young()` evacuates: roots (stacks, handles, dirty cards)
  trace the young space; live young objects are copied into old `Bump`
  regions (full promotion — no survivor spaces; aging is not in the
  contract); the young space resets to empty. Old→young edges are found by
  walking the dirty cards' REGION OBJECT CHAINS precisely: every region
  with a dirty card has its object chain scanned (klass/length/size are
  immutable header data; the chain is contiguous because old space is
  never freed per-object), and each tagged slot with a young target is a
  root edge recorded for the in-place update pass. All three legs (stacks,
  handles, cards) are precise — no conservative scanning anywhere.
* Trigger: young collection runs synchronously inside the allocation slow
  path (Rule 82 — the VM never aborts on ordinary allocation failure); a
  failed evacuation (`OutOfMemory`) surfaces as the named allocation error.

### 8.2 Object header v2 — the Brooks pointer

```cpp
struct ObjectHeader {
    Klass* klass;        // +0   immutable after allocation
    uint32_t size;       // +8   rounded allocation size; +flags word:
                         //        mark:1 (SATB marking), forwarded:1,
                         //        pinned:1 (FFI pin), reserved:29
    HeapObject* brooks;  // +16  SELF after allocation; the new body after
                         //        evacuation (docs section 4: Brooks pointer)
};
```

The header grows 16 → 24 bytes. Consequences (all verified by tests):
* Every allocation site initializes `brooks = self` (C++ allocators, the
  J1 `NEW_OBJECT`/`NEW_ARRAY`/boxed-double stencils, any inline bump in
  J2-J4 emission).
* `klass`, `size`, `length` are IMMUTABLE — reads may use either body.
  Field/element/payload data moves with the body: every data access in the
  mutator resolves through the Brooks pointer first
  (`resolve(o) = o->header.brooks`; predicted-taken `== self` fast path).
* The stencil corpus and J2/J4 emission derive every layout offset from
  `sizeof(ObjectHeader)` / `kArrayHeaderBytes`; the three baked literal-16
  array-length offsets and the `PA_FieldByteOff 16 + 8*slot` patch become
  named-constant arithmetic (CEM-26 section 2).

### 8.3 Write barrier v2 — card + SATB + remembered set

One entry point, three legs (docs section 3; Rules 79/80):

```text
barriered_store(owner, slot_addr, value):
  1. resolve owner through Brooks; recompute slot in the resolved body
  2. old = *slot
  3. if marking_active: log old into the SATB buffer        (SATB leg)
  4. *slot = value
  5. card_table[slot_addr >> kCardShift] = DIRTY            (card leg)
  6. if region(slot) != region(value target): append slot_addr to the
     target region's remembered set                          (remset leg)
```

* T0 and the J1/J2-J4 helper paths route every reference store through the
  barrier (the C++ `Heap::barriered_store`).
* JIT direct stores (J1 `SET_FIELD`/`ARRAY_SET` stencils, J2-J4 emitted
  `ArrayElement`/`RawOffset` stores) keep the inline fast path — plain
  store + card — and gain: (a) Brooks resolution of the base before the
  address computation, (b) a marking-active byte test that diverts to the
  outlined barrier helper (`J1Context::barrier_store`, 0xE0) when the
  concurrent marker is live (Rule 79: missing barriers are blocker bugs;
  the outlined form is docs section 3's "inline barrier, outlined
  barrier"). Steady-state cost: 1 load + cmp + jcc for the resolve,
  1 byte load + test + jcc for the SATB check, both predicted not-taken.
* Barrier elision (Rule 80) is NOT claimed anywhere in M5 — every
  reference store executes the barrier legs. Scalar-replaced allocations
  never materialize, which is EA elision by construction (proven form).

### 8.4 The old-generation cycle

`IcggcEngine` owns the phase machine. Phases advance only in engine calls;
the mutator runs between engine calls (the stepwise-driver model, the same
concurrency contract as the J4 worker — `docs/tier-j4.md` 12.3; M6 threading
moves slices to a background thread without redesign):

```text
Idle --start_cycle--> InitialMark (STW pause #1: SATB snapshot flip,
                      roots scanned and enqueued; pause ~ O(roots))
     --step()--> ConcurrentMark (slices: drain the mark queue; the mutator
                      runs between slices and its stores log SATB)
     --step()--> FinalMark (STW pause #2: drain SATB tail, per-region live
                      bytes, CSet selection, weak-handle decisions;
                      pause ~ O(SATB tail + region table))
     --step()--> Evacuate (slices: copy one CSet region per slice — region
                      granularity is atomic between slices: copy live
                      objects, install Brooks forwarding, update that
                      region's remembered set and all young slots)
     --step()--> UpdateRoots (STW pause #3: JIT frames via GC maps, T0
                      frames, handles — stale oops resolved through Brooks;
                      pause ~ O(roots))
     --step()--> Sweep (slices: free fully-dead regions, reclaim humongous
                      runs, reset CSet regions)
     --step()--> Idle
```

* SATB correctness: the initial-mark pause records the root set (the
  snapshot) AND clears the young mark bits (consecutive cycles with no
  young GC between them would otherwise skip every young body via the
  mark dedup and lose its old referents). During ConcurrentMark the
  mutator may overwrite any reference; the SATB leg preserves the
  snapshot's reachable set (tri-color via SATB, docs section 3.1).
  Allocations during the cycle land in the young space, which the old
  mark treats as a root region — no TAMS machinery. The FinalMark
  assertion mode (8.4 below) fails the cycle by name when the closure
  breaks.
* Evacuation safety between slices: a mutator oop pointing at an evacuated
  object resolves through the Brooks pointer — loads AND stores land in the
  new body; the old body is frozen (never written after forwarding) and is
  reclaimed only in Sweep (quiescence, Rule 90 analog). Region-granularity
  copy keeps every region either fully forwarded or untouched between
  slices, so no half-state is observable.
* Triggers: `maybe_start_cycle()` when allocation-based old occupancy
  crosses `IcggcConfig::old_gc_trigger_pct` (75%); the driver calls it
  between mutator executions. (The adaptive allocation-pressure trigger is
  M6 — the shipped trigger is occupancy-only.)
* Driver obligation: a young collection NEVER runs inside a live old cycle
  (`collect_young` returns a named `InvalidState`), so the driver must let
  a cycle reach Idle before mutator allocation pressure can force young
  evacuation. M5's drivers drain cycles between mutator batches (the
  stress suite's interleave shape).
* Assertion mode (`IcggcConfig::assertion_mode`, tests/stress only): the
  FinalMark pause verifies the SATB closure — every object reachable from
  the recorded snapshot roots must be marked. A missed SATB log (a barrier
  that skipped its SATB leg while marking was active) leaves a
  snapshot-reachable object white and fails the cycle with the named error
  `SatbClosureViolated`. This is the DoD's "no missed SATB logs" gate and
  it exercises the REAL JIT barrier paths end-to-end.

### 8.5 Incremental pacing

The pacer (docs section 6) computes each slice's budget from:

* allocation rate (bytes since the last slice),
* live-set growth (per-cycle live bytes vs the previous cycle),
* old-space occupancy,
* the pause budget `kPauseBudgetNs` (1 ms; slices and STW phases are both
  pause events — the DoD's p99 is computed over ALL of them).

Slice work units: `IcggcConfig::mark_slice_items` mark-queue pops, one CSet
region copy, `IcggcConfig::sweep_slice_regions` region reclaims. The pacer
never terminates a cycle early and never skips a phase (Rule 77: pause
bounding, not cycle bounding). (The shipped sizing is fixed config units —
the adaptive rate/growth inputs are M6 threading work.)

### 8.6 GC-map-driven precise stack scanning for JIT frames

Root scanning at every pause (and the UpdateRoots phase) walks:

1. **T0 frames** — `Interpreter::frames_[0, frames_live_)` register files,
   precise (every TaggedValue slot).
2. **JIT frames** — the RBP chain (`docs/tier-j1.md` ABI; the J2-J4
   emitter keeps the same `push rbp; mov rbp,rsp` prologue, fast_jit.cpp
   prologue). Every GC-capable helper (alloc_slow, the invoke paths)
   stores the innermost JIT frame's rbp into `J1Context::gc_frame_rbp`
   (0x50) and captures its OWN return address into `gc_frame_pc` (0x60) —
   the anchor frame's identity pc — with one @cold store per slow path, so
   the scan needs no C++ frame-pointer assumptions. The walk: the ANCHOR
   frame is scanned first (its record resolves through gc_frame_pc), then
   each caller frame through its callee's return address at [rbp + 8] →
   the published-executable registry (`GcCodeRegistry`: code range →
   gc_maps + slot geometry) → J1 frames scan ALL vregs (every vreg slot is
   a TaggedValue; the heap-tag check makes the conservative-all published
   map precise) / J2-J4 frames decode the per-safepoint maps (`site u32,
   words u32, bits u32[]`, Rule 86 metadata; the return address belongs to
   the nearest preceding safepoint site) → each set bit is one frame slot
   holding a reference → resolve + mark/update. JIT-to-JIT calls link RBP
   chains naturally; the walk stops at the first pc outside every
   registered code range (the C++ driver boundary — C++ frames hold no
   unassisted roots; the runtime invariant "C++ frames never hold the only
   reference across a GC point" is stated in 8.8).
3. **Handles** — `HandleTable` entries; Global/Local are roots, Weak
   handles are cleared at FinalMark when their object is unmarked
   (Rule 85; `resolve` then returns nullptr by name).
4. **Pinned objects** — every active `PinningRegion` (FFI, docs
   infrastructure/05): pinned objects are marked as roots AND excluded
   from evacuation (their region stays out of the CSet; `header.pinned`
   names the state for diagnostics). Rule 84: pinned addresses are the
   stable-identity mechanism.

### 8.7 Identity, forwarding, and moving semantics (Rule 84)

* Guest identity is value-field equality plus object identity through the
  resolved body; after evacuation every reference (heap slots, stack oops,
  handles) points at the new body — the frozen old body is unreachable and
  swept. No guest-visible address escapes: JIT code materializes object
  addresses only in registers/frame slots, all covered by GC maps.
* Weak handles to dead objects clear (nullptr) — Rule 85's valid-state
  boundary.
* Humongous objects never move; a pinned object never moves.

### 8.8 Runtime invariants M5 relies on (and tests pin)

1. C++ runtime frames never hold the only reference to an object across a
   GC point (T0 registers, JIT frame slots, and handles are the root
   universe; the ASan/UBSan + stress suite validates it empirically).
2. Region-granularity slice atomicity (8.4) — no half-evacuated region is
   observable between engine steps.
3. The mutator runs only between engine calls in M5 (stepwise driver);
   barrier paths are already thread-ready state machines for M6.
4. One mutator thread (M0 contract) — the engine takes no locks; M6 adds
   the handshake.

### 8.9 DoD (docs/roadmap.md M5)

* `gc_pause_p99_under_budget_on_allocation_stress` — a long mixed
  allocation/mutation workload through T0 AND published JIT code, young
  and old cycles driven by the triggers; every pause event (young GCs,
  STW phases, slices) recorded with ns timestamps; p99 < `kPauseBudgetNs`.
* `gc_satb_closure_holds_with_jit_stores_during_marking` — assertion-mode
  cycle with live JIT stores against marked objects (the J1 stencil
  barrier path); the SATB closure must hold (8.4). A deliberately
  barrier-bypassed store (the regression shape: a store that skips the
  SATB leg) must fail the closure — the negative test proves the gate
  detects misses.
* `gc_compaction_preserves_identity_under_ffi_pinning` — pinned objects
  keep their addresses across cycles (identity observable through the
  handle), unpinned objects evacuate (addresses move), all reads/writes
  after the cycle observe the evacuated state, weak handles to dead
  objects resolve to nullptr.
