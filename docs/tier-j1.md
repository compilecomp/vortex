# J1 — Stencil Baseline JIT

J1 exists to escape the interpreter quickly. It produces low-quality but safe
native code at near-instant compile latency. It is not intended to be fast forever.

---

## 1. Baseline philosophy

J1 has **no IR** — no AST, no CFG, no SSA, no Sea-of-Nodes, no MIR, no LIR:

```text
bytecode -> stencil selection -> stencil instantiation -> patched machine code
```

Design targets: low compile latency, concurrent compilation, predictable code
shape, cheap speculation, direct IC embedding.

---

## 2. Stencils

A stencil is a precompiled machine-code template for one bytecode or one bytecode
pattern:

```text
ADD_I32_STENCIL:
    mov  src1, [frame + vreg1]
    add  src1, src2
    jo   overflow_trap
    mov  [frame + dst], src1
    update profile counter
    advance bytecode PC
```

Stencil structure:

```text
Stencil:
    template_bytes
    template_size
    patch_sites[]
    ic_slots[]
    gc_map_offset
    deopt_record_offset
    metadata_offset
```

Patch site types:

```text
VIRTUAL_REGISTER, CONSTANT_INDEX, IC_SLOT, BRANCH_TARGET, CALL_TARGET,
CARD_TABLE_BASE, THREAD_LOCAL_SLOT, PROFILE_COUNTER, BYTECODE_PC, DEOPT_HANDLE
```

---

## 3. Superstencils

A superstencil is a fused stencil for multiple bytecode operations:

```text
LOAD_FIELD + ADD_I32 + STORE_LOCAL  ->  SUPERSTENCIL_LOAD_ADD_STORE
```

Benefits: fewer dispatches, fewer PC updates, fewer counter updates, better
register locality, fewer barriers, better branch layout. Superstencils are
generated offline or promoted dynamically from hot bytecode bigrams/trigrams.

---

## 4. Stencil instantiation

```text
1. Allocate executable buffer.
2. Select stencil or superstencil for each bytecode.
3. Copy stencil bytes.
4. Patch runtime values.
5. Emit IC slots.
6. Emit GC maps.
7. Emit deopt metadata.
8. Make code executable (W^X flip).
9. Publish with memory barrier.
```

Instantiation is parallel per method.

---

## 5. Multi-threaded baseline compilation

```text
compilation queue -> work stealing -> per-method compile task
    -> thread-local code allocator -> lock-free publication
```

Safety: no mutable shared graph, atomic IC installation, memory barriers before
code publication, code installation via atomic method-table patch.

---

## 6. Baseline speculation

J1 consumes interpreter profile data directly and emits:

- typed stencils when the profile is stable,
- generic stencils when the profile is unstable,
- IC-guarded field accesses,
- direct calls for monomorphic call sites,
- polymorphic dispatch for small type sets,
- megamorphic stubs for unstable sites.

Typed stencil failure path: increment failure counter, jump to fallback stencil or
trigger deopt/OSR.

---

## 7. Guard patch points

Typed stencils reserve patchable guard space:

```text
nop nop nop nop   ->   test klass, expected ; jne failure
```

This allows cheap guard strengthening without full recompilation.

---

## 8. Baseline OSR and deopt

Baseline code contains OSR entries for hot loops (bytecode PC, register mapping,
IC state, stack layout, GC root map) and can OSR into J2. Baseline deopt records
are compact:

```text
BaselineDeoptRecord:
    bytecode_pc
    frame_descriptor_id
    register_map
    ic_state_snapshot
```

They support full deopt to interpreter, partial fallback within baseline, OSR exit,
and GC scanning.

---

## 9. J1 optimization inventory

Only cheap optimizations: stencil selection, superstencil fusion, IC embedding,
direct call patching for monomorphic sites, local register mapping, inline barrier
emission, minimal guard patch points, baseline OSR stubs, compact deopt metadata,
GC root map generation.

## 10. Compile budget

```text
time:     near-instant
memory:   code bytes + minimal metadata
analysis: none or extremely local
```

Status in this milestone: **complete** — the x86-64 corpus (68 templates) is
assembled once at build time (`src/j1/stencil_corpus.cpp`), instantiated by
memcpy + patch against the ABI in `include/vortex/j1/context.hpp`, and the
parity DoD holds: J1-compiled methods produce T0-identical observable results
(tests/test_j1.cpp). Superstencil promotion and IC guard strengthening are
exercised by tests; profile-driven guard strengthening at instantiation lands
with the M2 plumbing.
