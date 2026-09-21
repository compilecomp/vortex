# T0 — Speculative Register Interpreter

T0 is the execution tier that starts immediately, profiles everything, rewrites
bytecode adaptively, and hands hot code to the JIT tiers via OSR. It is **not
counted as a JIT tier**.

---

## 1. Interpreter shape

T0 is a **register interpreter**, not a stack interpreter. Each interpreter frame
contains:

```text
Frame:
    return PC
    caller frame pointer
    method metadata pointer
    bytecode PC
    virtual register file
    local IC slot pointer
    profile counters pointer
    GC root map ID
    OSR state
```

Virtual registers are tagged machine words:

```text
VReg:
    bits : 64-bit payload
    tag  : type/tag metadata
```

Tag representations (selected per target architecture and object model): explicit
tag byte, tagged pointer, compressed class reference, NaN-boxed value, hardware
pointer tag. Vortex ships the tagged-pointer scheme in `include/vortex/support/
tagged_value.hpp`.

---

## 2. Speculative bytecode

The bytecode has typed (speculative) and untyped (generic) forms.

Typed speculative instructions — assume the profile is correct:

```text
ADD_I32            dst, src1, src2
ADD_F64            dst, src1, src2
LOAD_FIELD_MONO    dst, obj, field_id, ic_slot
STORE_FIELD_MONO   obj, field_id, val, ic_slot
CALL_DIRECT_FAST   target, argc
CHECK_CLASS        obj, expected_klass
CHECK_BOUNDS       idx, length
```

Generic fallback instructions — handle all cases, slower:

```text
ADD_ANY, LOAD_FIELD_POLY, LOAD_FIELD_MEGA, STORE_FIELD_POLY,
CALL_GENERIC, CHECK_CLASS_POLY
```

A failing typed instruction increments its failure counter and falls back to the
canonical semantics; chronic failure triggers adaptive rewriting (section 6).

---

## 3. Inline caches

Every call/field/type site has an IC slot with atomic, thread-safe transitions:

```text
MonoIC:   cached_klass_or_shape, target_or_offset, failure_count
PolyIC:   entry_count, entries[4], megamorphic_stub
MegaIC:   lookup_stub

uninitialized -> monomorphic -> polymorphic -> megamorphic
```

---

## 4. Dispatch

High-performance dispatch: computed goto, label table, direct threaded dispatch,
cache-line-aligned handlers, next-bytecode prefetch, superinstruction handlers.
Dispatch table: `dispatch_table[opcode] -> handler_label`. Hot handlers may be
duplicated to reduce I-cache pressure. The reference implementation
(`src/vm/interpreter.cpp`) uses a computed-goto core with a switch fallback under
`VORTEX_NO_COMPUTED_GOTO`.

---

## 5. Superinstructions

Common sequences are fused into single operations:

```text
LOAD_FIELD_I32_ADD_I32
LOAD_LOCAL_CHECK_NULL_LOAD_FIELD
ADD_I32_STORE_LOCAL
CALL_DIRECT_CHECK_RETURN
```

Superinstructions reduce dispatch overhead, register file traffic, profile counter
updates, and IC checks. The adaptive rewriter performs hot-bigram detection and
counting today; in-place opcode re-typing (canonical <-> typed) is implemented,
and stream-level superinstruction re-encoding lands with the J1 stencil corpus
(see `docs/roadmap.md`, M1).

---

## 6. Profiling and adaptive rewriting

Per-bytecode-index profile storage:

```text
ProfileSlot:
    execution_count
    failure_count
    type_ring[4]
    shape_ring[4]
    branch_counts[2]
```

Recorded: method invocation count, loop backedge count, branch taken/not-taken,
receiver/argument/return types, field shapes, allocation sites, per-instruction
failure counts, IC transitions, deopt-like fallback events.

Rewriting rules (atomic and safe):

```text
typed instruction repeatedly succeeds        -> keep
typed instruction fails occasionally         -> keep, increase failure count
typed instruction fails chronically          -> rewrite to generic form
poly IC becomes megamorphic                  -> rewrite to megamorphic form
bytecode pair hot and superstencil exists    -> rewrite to superinstruction
```

---

## 7. OSR

OSR entry points exist at loop headers, call sites, and long-running bytecode
regions. OSR state: bytecode PC, virtual register contents, IC state, profile
state, loop depth. The interpreter transfers to compiled code mid-execution by
materializing the interpreter frame into the OSR entry layout of the target tier.

---

## 8. GC integration

T0 provides: precise root maps per bytecode PC, a tagged register file, inline
write barriers (card marking: `card_table[obj >> CARD_SHIFT] = DIRTY`), TLAB
allocation fast paths, safepoint polls at backward branches, and lazy stack
scanning support.

---

## 9. Tier handoff

T0 consults the tiering policy (`include/vortex/runtime/tiering.hpp`) on method
entry and back-branches. When invocation/backedge thresholds fire, T0:

1. freezes and transfers profile + IC state to the compile job,
2. registers OSR entry points,
3. continues executing until compiled code is installed and atomically patched in,
4. on OSR entry, materializes its frame into the target tier's OSR layout.

Compile budget: zero. T0 never blocks on compilation.
