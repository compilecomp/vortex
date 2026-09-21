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

## 8. Implementation status

In this milestone the following are implemented and tested: TLAB bump allocator
with header initialization, card table with dirty marking, tagged object header,
and the heap layout scaffolding. The concurrent marking/sweeping/evacuation engine
is contract-stubbed (see `docs/roadmap.md`, milestone G1–G3).
