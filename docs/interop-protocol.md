# The Interop Message Protocol

This replaces the grab-bag `Environment` concept with a uniform,
capability-gated message surface that the devirtualization graph and
structural hashing can operate on canonically. It is the substrate the
LDPT trampolines (docs/ldpt.md) and Cross-Language Escape Analysis
(docs/xlea.md) specialize.

Status in this milestone: **spec complete** — the message model, the
capability bits and the UGB opcode mapping are defined and reflected in
`include/vortex/runtime/interop.hpp` and the UGB opcode table
(`POLY_EXECUTE`/`POLY_READ`/`POLY_WRITE`/`POLY_SEND`). Implementation
lands with the J3 devirtualization pipeline (docs/roadmap.md M3/M4).

---

## 1. Design principles

1. **Message-based, not method-based.** One `Send` dispatcher plus a
   canonical message set — not bespoke virtuals per operation. A guest
   object from any language is driven through the same messages, so the
   devirt graph sees one shape of dispatch to specialize.
2. **Capability-gated.** Every message maps to a UGB capability bit;
   unsupported messages return `UNSUPPORTED`, enabling safe fallback
   (Rule 3: never silent misexecution).
3. **Tier-specializable.** Hot messages get dedicated UGB opcodes so
   J1–J4 can devirtualize and inline them; cold messages use a generic
   send.

## 2. Core message set

| Category | Messages |
| :--- | :--- |
| **Structure** | `HAS_MEMBERS`, `READ_MEMBER`, `WRITE_MEMBER`, `REMOVE_MEMBER`, `MEMBERS`, `IS_MEMBER_READABLE`, `IS_MEMBER_WRITABLE` |
| **Invocation** | `IS_EXECUTABLE`, `EXECUTE`, `IS_INSTANTIABLE`, `INSTANTIATE` |
| **Array** | `HAS_ARRAY_ELEMENTS`, `READ_ARRAY_ELEMENT`, `WRITE_ARRAY_ELEMENT`, `ARRAY_SIZE` |
| **Type** | `GET_META_OBJECT`, `IS_META_INSTANCE`, `GET_META_QUALIFIED_NAME` |
| **Unbox** | `IS_BOOLEAN`/`AS_BOOLEAN`, `IS_NUMBER`/`AS_NUMBER`, `IS_STRING`/`AS_STRING`, `IS_NULL` |
| **FFI** | `IS_POINTER`, `AS_POINTER` |
| **Exception** | `IS_EXCEPTION`, `THROW`, `GET_EXCEPTION_TYPE` |

Message ids are a closed enumeration in `runtime/interop.hpp`
(`InteropMessage`); unknown ids from newer producers behave like unknown
UGB capabilities: safely rejectable, never silently misexecutable.

## 3. Capability mask (UGB integration)

```cpp
enum InteropCapability : uint32_t {
    CAP_INTEROP_MEMBERS    = 1 << 0,
    CAP_INTEROP_EXECUTE    = 1 << 1,
    CAP_INTEROP_ARRAY      = 1 << 2,
    CAP_INTEROP_PRIMITIVES = 1 << 3,
    CAP_INTEROP_POINTER    = 1 << 4,
    CAP_INTEROP_EXCEPTION  = 1 << 5,
};
```

Loader validation: if a module emits `POLY_EXECUTE`/`POLY_SEND(EXECUTE)`,
it must declare `CAP_INTEROP_EXECUTE`; likewise for the other groups.
The validation mirrors UGB Rule 3 — the capability set is negotiated at
load, and a missing bit is a load-time rejection, not a runtime surprise.

## 4. Per-language message vtable

Each registered language port installs one flat vtable; null slots mean
"this language does not support the message" (the dispatcher returns
`UNSUPPORTED`):

```cpp
struct InteropVTable {
    HasMembersFn   has_members;    // null if unsupported
    ReadMemberFn   read_member;
    WriteMemberFn  write_member;
    ExecuteFn      execute;
    InstantiateFn  instantiate;
    ReadArrayFn    read_array;
    UnboxFn        unbox;
    // ... one slot per message
};
```

Vtable slots are plain function pointers registered once at language load
(@cold registration; the hot path is the dispatcher + the JIT
specializations below). Strings never appear on hot paths — member
access resolves through member_idx tokens (Rule 5).

## 5. UGB opcode mapping

Hot messages get dedicated opcodes for JIT specialization; cold ones use
a generic send:

```nasm
POLY_EXECUTE   target, args, ret       ; hot path (dst, arg_base, argc)
POLY_READ      recv, member_idx, ret   ; hot path
POLY_WRITE     recv, member_idx, val
POLY_SEND      msg_id, recv, args      ; cold/generic fallback
```

`POLY_*` opcodes are speculative by construction: their fast path is the
devirtualized direct dispatch once the receiver's language/klass is
known, and their fallback is the vtable send. This is exactly the shape
the T0 adaptive rewriter and the J1 speculative-stencil corpus already
implement for arithmetic — the interop messages reuse the machinery.

## 6. Tier progression for a message

- **T0**: calls `vtable.execute(recv, args)` through the context's
  function-pointer slot (the same indirect-call contract as
  CALL_BUILTIN, PERF-002); records feedback (language id + member idx)
  in the IC/profile slots.
- **J1**: the stencil checks the recorded `LangID`, and jumps to the
  pre-generated execute trampoline for that language — the same
  IC-guard + slow-fallback shape as GET_FIELD (docs/tier-j1.md
  section 7; `patch_ic_guard` strengthens it at instantiation).
- **J3/J4**: if the receiver is monomorphic (from the devirt graph), the
  optimizer **inlines the target language's handler directly, bypassing
  the vtable entirely** — and, under XLEA (docs/xlea.md), collapses the
  surrounding wrapper allocation and member dispatches into plain SSA
  value flow.

The synthesis: the protocol gives a canonical message surface; the
devirt graph specializes it into direct calls.

## 7. Relationship to existing machinery

- The `POLY_*` fast/fallback pair is the same speculative-stencil shape
  as the M1 corpus's typed arithmetic (guard → fallback → helper).
- The per-language trampolines are LDPT sites: language id plays the
  role of the klass word in the skeleton compare, and the escalation
  ladder (mono/poly/mega) applies unchanged.
- `UNSUPPORTED` propagation uses the standard `Result`/Diagnostic
  channel with a dedicated error id, so ports surface it uniformly.
