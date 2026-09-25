# UGB — Universal Guest Bytecode

UGB is the portable contract between guest-language frontends and the Vortex
runtime. It is:

```text
register-based
typed-but-optional
semantically explicit
profile-friendly
specialization-friendly
GC-aware
deopt-aware
capability-extensible
```

It is not a stack bytecode, not a raw machine ISA, and not a language AST. It is a
portable executable semantic representation. The JIT never needs to know the guest
source language.

---

## 1. Design goals

**Language neutrality.** UGB does not assume Java classes, JavaScript prototypes,
Python magic methods, C++ vtables, Rust traits, Lisp macros, C pointer semantics,
Smalltalk message passing, or Haskell laziness as core mandatory semantics. It
provides generic mechanisms instead: calls, dispatch descriptors, type tokens,
shape tokens, field tokens, method tokens, runtime hooks, and capability
extensions.

**Transferability.** A JIT can say "I support UGB version X with capabilities
A, B, C". A frontend can say "I emit UGB version X and require capabilities D, E".
If the required capabilities are supported, the code runs.

**Coverage.** UGB covers static, dynamic, class-based, prototype-based, functional,
scripting, systems, managed, concurrent, async/coroutine languages and languages
with exceptions, closures, FFI, dynamic dispatch, operator overloading, reflection,
and metaprogramming hooks — through core instructions, typed specializations,
metadata tokens, runtime hooks, and optional capability extensions.

---

## 2. Register model

Each method declares virtual registers `v0, v1, ..., vN`. A register can hold
tagged values, unboxed primitives, object references, closure references,
raw/foreign pointers, type tokens, shape tokens, method handles, continuation
handles, or vector values.

Register classes:

```text
ANY  I1  I8  I16  I32  I64  U8  U16  U32  U64
F32  F64  F128(optional)  V128(optional)  V256(optional)
REF  CLOSURE  COROUTINE  CONTINUATION  FOREIGN
TYPE  SHAPE  METHOD  UNIT
```

The interpreter uses a tagged register file; the JIT can unbox registers based on
profiles.

---

## 3. Method container format

```text
UGBMethod:
    header
    capability list
    argument descriptor
    register descriptor
    constant pool
    metadata pool
    type token table
    shape token table
    field token table
    method token table
    code stream
    exception table
    profile table
    IC table
    debug table
    language metadata
```

### 3.1 Header

```text
Header:
    magic, version_major, version_minor
    language_id, module_id, method_id
    flags
    register_count, argument_count, max_stack_metadata
    code_offset, exception_table_offset
    profile_table_offset, debug_table_offset
```

Flags: `HAS_TYPED_REGS, HAS_DYNAMIC_SITES, HAS_CLOSURES, HAS_EXCEPTIONS,
HAS_COROUTINES, HAS_TAIL_CALLS, HAS_UNSAFE_MEMORY, HAS_FFI, HAS_ATOMICS,
HAS_VECTOR_OPS, REQUIRES_EXACT_GC, REQUIRES_GUARANTEED_TCO`.

### 3.2 Capability list

Methods declare required capabilities; the JIT advertises supported ones. Required
but unsupported capabilities force a guest-runtime fallback, method rejection, or
interpreter-hook execution.

Core capabilities: `core, typed_arithmetic, checked_arithmetic, generic_dispatch,
class_dispatch, interface_dispatch, prototype_dispatch, multimethod_dispatch,
closures, exceptions, resumable_exceptions, tail_calls, coroutines, async, atomics,
vector, unsafe_memory, ffi, reflection, dynamic_eval, big_numeric,
decimal_numeric, string_ops, collection_ops, gc_barriers`.

Capability packs (`ugb.pack.*`) bundle these for advertisement: `dynamic_objects,
class_objects, prototype_objects, closures, exceptions, tail_calls, coroutines,
async, atomics, vector, unsafe_memory, ffi, reflection, big_numeric,
decimal_numeric, string_ops, collections`.

Conformance levels: `UGB-Core-Interpreter`, `UGB-Baseline`, `UGB-FastOpt`,
`UGB-FullOpt`, `UGB-MaxOpt`.

---

## 4. Instruction model

A canonical instruction is `OP dst, src1, src2, metadata_token`. The standard
defines opcode semantics, operand classes, trap conditions, fallback behavior,
profile site behavior, GC safety rules, and deopt state requirements.

### 4.1 Canonical vs specialized instructions

This distinction is essential for speculative JIT operation.

```text
canonical:     Add.Any       v3, v1, v2, site_id
specialized:   Add.I32       v3, v1, v2, fallback_site_id
               Add.I64 / Add.F64 / Add.TaggedInt ...
```

A specialized form is valid only if operand types, overflow behavior, language
fallback semantics, and the trap/deopt path are guaranteed. If a specialization
fails, execution continues with canonical semantics. This single rule is what
supports speculative bytecode, interpreter rewriting, baseline stencils, J2/J3/J4
optimization, and partial deopt with one shared semantic.

### 4.2 Instruction categories

| Category | Representative opcodes |
|----------|------------------------|
| Constants & moves | `Const.Unit/Bool/I8..I64/U8..U64/F32/F64/String/Symbol/Type/Shape/Method/Null/Foreign`, `Move`, `Swap`, `Copy` |
| Integer arithmetic | `Add/Sub/Mul.I8..I64`, `Div.S/U`, `Rem.S/U`, `Neg.I`, `Abs.I`, checked variants `Add.Checked.I32` ... |
| Float arithmetic | `Add/Sub/Mul/Div/Rem.F32/F64`, `Neg/Abs/Sqrt/Min/Max`, flags `IEEE754, FAST_MATH, NO_NAN, NO_INF, DETERMINISTIC` |
| Bit operations | `And/Or/Xor/Not/Shl/Shr.S/Shr.U/RotL/RotR`, `BitCount`, `LeadingZeros`, `TrailingZeros`, `ByteSwap`, `BitReverse` |
| Comparisons | `Eq.Bool/I/F/Ref/Null`, `Ne/Lt/Le/Gt/Ge` (signed/unsigned/float), `Compare.Ordered/Total/Any` |
| Conversions | `SExt/ZExt/Trunc.*`, `IntToFloat.*`, `FloatToInt.*`, `Bitcast.*`, `Box`, `Unbox`, `AnyToTyped`, `TypedToAny`, policies `Checked/Truncating/Saturating/FallbackHook` |
| Control flow | `Jump`, `JumpTrue/False/Eq/Ne/Lt/Le/Gt/Ge`, `Switch.Int/Range/Hash`, `LookupSwitch`, `Return`, `Return.Unit`, `Unreachable`; branch metadata `likely`, `profile_counter_id`, `osr_site_id` |
| Calls & dispatch | `CallDirect`, `CallVirtual`, `CallInterface`, `CallProtocol`, `CallTrait`, `CallDynamic`, `CallClosure`, `CallIndirect`, `CallBuiltin`, `CallForeign`, `CallSuper`, `CallSpecial`, `CallMultimethod`, tail-call variants |
| Arguments | `PrepareArgs`, `SetArg`, `SetArg.Varargs`, `CollectVarargs`, `SpreadArgs`, `NamedArgs.Bind`; varargs strategies `NONE, ARRAY_PACKED, LIST_BUILDER, RUNTIME_COLLECT` |
| Allocation | `New.Object`, `New.Object.Shape`, `New.Object.Klass`, `New.Struct`, `New.Value`, `New.Array`, `New.Buffer`, `New.Closure`, `New.Coroutine`, `New.Continuation`, `New.Foreign` with `allocation_site_id` |
| Field access | `GetField`, `SetField`, `GetField.Shape`, `SetField.Shape`, `GetField.Offset`, `SetField.Offset`, dynamic `GetProp.Dynamic`, `SetProp.Dynamic`, `DeleteProp.Dynamic`, `HasProp.Dynamic`, symbol forms |
| Arrays & collections | `Array.New/Length/Get/Set`, checked and unchecked variants, `Array.Slice/Copy/Fill`, optional `Vector.*`; collection extension pack `Map.*`, `Set.*`, `Deque.*` |
| Strings | `String.New/Length/Concat/Slice/Eq/Compare/CodePointAt` + `string_ops`/`unicode_ops`/`string_interning` extensions |
| Closures | `Closure.New/GetUpvalue/SetUpvalue/Close/Self/Env` with `CaptureDescriptor` (by-value/by-reference masks, escape flags) |
| Exceptions | `TryBegin/End`, `CatchBegin`, `FinallyBegin/End`, `Throw`, `Rethrow`, `Unwind`, optional `Resume`; exception table entries with catch type, handler state, `is_catch/is_finally/is_resumable` |
| Type checks & guards | `Check.Null/NonNull/Type/Shape/Klass/Interface/Protocol/Bounds/Overflow/ArrayLength/Readonly/Final/Initialized`, each carrying `guard_site_id`, `fallback_target`, `deopt_record_id`, `profile_id` |
| Atomics & concurrency | `Atomic.Load/Store/Add/Sub/And/Or/Xor/Exchange/CompareExchange`, `Fence.Acquire/Release/SeqCst`, `ThreadLocal.Get/Set`, `Safepoint`, `CheckInterrupt` |
| Async & coroutines | `Coroutine.New/Resume/Yield/Suspend/IsDone`, `Await`, `SuspendPoint`, optional `Continuation.Capture/Resume`; suspend metadata carries frame id, live registers, GC roots, resume PC |
| Unsafe memory & FFI | `Raw.Load/Store.I8..F64`, `Raw.Alloc/Free`, `Pointer.Add/Sub/Cast/IsNull`, `Foreign.Declare/Call/Callback/StructLoad/StructStore` |
| GC barrier hints | `WriteBarrier.Store/Initialize/Publish`, optional `ReadBarrier.Load` on `SetField.Ref`, `Array.Set.Ref` |
| Reflection | `Meta.GetType/GetShape/GetMethodName/LookupSymbol/ResolveMethod/HasCapability/GetDebugInfo`, `Meta.LoadModule/LinkMethod/PatchCallSite` |
| Debug | `Debug.SourcePosition/LocalName/Trap/Breakpoint/Unreachable` |

### 4.3 Call descriptor

```text
CallDescriptor:
    method_token
    dispatch_kind: STATIC | DIRECT | VIRTUAL | INTERFACE | PROTOCOL | TRAIT
                 | PROTOTYPE | DYNAMIC | MULTIMETHOD | CLOSURE | FOREIGN | BUILTIN
    argument_count
    varargs_mode, named_args_mode
    return_type_token
    side_effect_class
    can_throw, can_allocate, can_suspend, can_deopt
```

One instruction shape therefore covers vtable calls, interface calls, prototype
lookup, `method_missing`, multimethod dispatch, closure invocation, and foreign
calls.

### 4.4 Object model abstraction

UGB mandates no object model. References are opaque `REF`s; metadata describes
GC-tracing, nullability, shape/klass presence, immutability, value semantics, and
movability.

```text
ShapeToken:     layout_id, parent_shape?, field_count, field_types,
                prototype_chain?, klass_id?, transition_table?
FieldToken:     field_name_or_id, owner_type_token, access_kind, visibility, flags
MethodToken:    method_id, owner_type, signature, dispatch_kind,
                implementation_kind: BYTECODE | NATIVE | INTRINSIC
                                    | RUNTIME_HOOK | MISSING_HOOK,
                capability_requirements, direct_target?
DispatchDescriptor: kind, receiver_type_token, method_token,
                fallback_hook, cache_site_id
```

Dynamic property access lowers to `shape guard -> offset load/store -> IC fallback`,
letting the runtime implement prototype chains, `__getattr__`, `method_missing`,
proxies, metaclasses, and property traps while the JIT optimizes the fast path.

---

## 5. Profiling, ICs, and specialization records

Every interesting site carries a stable ID: `call_site_id, field_site_id,
type_site_id, branch_site_id, allocation_site_id, conversion_site_id,
binary_op_site_id`.

```text
ProfileRecord:  site_id, kind, counter, type_history, shape_history,
                branch_counts, failure_count
ICRecord:       site_id, kind, mono_entry, poly_entries, mega_stub
SpecializationRecord:
    site_id
    canonical_opcode -> specialized_opcode
    assumption_set (lhs_type = I32, overflow = trap, null_checked = true,
                    shape = shape_token, ...)
    fallback_target
    deopt_record_id
```

These records are what allows the interpreter, the baseline JIT, and the optimizing
JITs to share specialization semantics portably.

---

## 6. Precise state

Each instruction has an implicit state descriptor for precise exceptions, GC,
partial deopt, OSR, debugging, and async suspension:

```text
InstructionState:
    pc, live_registers, gc_root_mask,
    exception_state, suspend_state?, deopt_record_id?
```

---

## 7. Extensions and versioning

Extensions are namespaced: `extension.<language>.<feature>` — e.g.
`extension.python.generator`, `extension.javascript.prototype`,
`extension.rust.drop_glue`, `extension.hardware.aes_ni`. An extension op is
encoded as `ExtOp namespace_id, op_id, operands..., metadata_token`. A JIT may
implement it natively, call a runtime hook, interpret it, or reject it.

UGB has a version (`version_major`, `version_minor`), stable capability IDs,
opcode IDs, metadata token formats, binary encoding rules, verification rules, and
conformance tests.

---

## 8. Binary encoding

Compact, fixed-width encoding:

```text
opcode:          u16   (core + extensions)
flags:           u8
dst:             vreg  u16
src_count:       u8
srcs:            vreg  u16[]
metadata_token:  u32   optional
```

The standard defines this distribution encoding, a compact variable-length encoding
for storage, and a wide internal encoding for interpreter speed. Vortex implements
the fixed-width form in `include/vortex/ugb/module.hpp` (`encode_module`,
`decode_module`, `InstructionStream`, `append_instruction`).

### 8.1 Implemented distribution format (v3)

The implemented wire format (format minor version **3**) records every artifact
versioning field the laws require (Rule 10) and the per-method capability lists
(Rule 3). Little-endian throughout:

```text
module:
  magic                 "UGB\0"
  version_major         u16    (0)
  version_minor         u16    (3)
  language_id           u32
  language_name         u32 length + bytes
  runtime_abi_version   u32    (1)          -- Rule 10
  metadata_schema_ver   u32    (1)          -- Rule 10
  extension_count       u32                 -- Rule 6
  extensions            count × { name: u32+bytes, version: u32 }
  capability_mask       u32                 -- Rule 3/10 (union of method needs)
  constants             u32 count × { kind u8 + payload }
                        (payload: Int64 = u64 two's complement;
                         Float64 = IEEE-754 bit pattern u64;
                         String = u32 length + bytes;
                         MethodRef = reserved u32)
  classes               u32 count × name
  fields                u32 count × { name, owner_class_token u32,
                                      declared u8 }              -- v3
  method_tokens         u32 count × name    (token = index + 1; 0 = none)
  builtin_tokens        u32 count × name
  methods               u32 count × {
      name, id u32, register_count u16, argument_count u16,
      cap_count u8 + capability ids u8[],          -- Rule 3
      code_length u32 + code bytes
  }
```

**Version law.** The decoder rejects incompatible major versions and minors
newer than the runtime, with a positional diagnostic (telemetry, Rule 9).
Minor-1 payloads decode with documented defaults: empty extension table,
empty capability set, `runtime_abi_version = metadata_schema_version = 0`.
Minor-2 payloads decode with `declared = true` for every field entry (the v2
layout contract).

**Declared fields (v3).** A field token enters the table either through the
`.field` directive (`declared = 1`) or through a bare instruction reference
(`declared = 0`). Only declared fields shape their owner klass's layout, so
an access to a referenced-only name resolves `find_field` -> miss -> the
canonical unresolved-field error (ADR-005 helper contract) instead of an
out-of-bounds slot. Token indices are load-bearing in instruction operands:
reference-only entries stay in the table, the flag is what excludes them
from layouts.

**Stable site IDs (Rule 8).** Every dynamic or speculative site (profile
slot, inline-cache slot, future deopt record) is identified by
`make_site_id(method_id, instruction_index)` — stable across runs for a
given (module, bytecode version). `UGBMethod::site_id(index)` is the
method-local form; the zero value `kNoSite` means "no site".

**Capability negotiation (Rule 3).** `engine_advertised_capabilities()`
returns the set the executing engine supports; `Interpreter::run` rejects —
safely, with a diagnostic — any method whose requirements are unknown or
unsupported. `docs/compliance_matrix.md` maps the checks; regression tests
live in `tests/test_compliance.cpp`.

---

## 9. Verification

Mandatory before execution. The verifier checks:

1. valid opcodes,
2. valid register uses,
3. registers defined before use,
4. control-flow targets valid,
5. exception handlers valid,
6. metadata tokens valid,
7. capabilities satisfied,
8. GC reference safety,
9. suspend-state consistency,
10. deopt state consistency,
11. call descriptors valid,
12. extension ops recognized or hooked.

---

## 10. What UGB does not do

UGB is not a source AST, not a complete language definition, not a universal type
system, not a universal memory model, not a scheduler, and not a macro system. The
guest runtime remains responsible for language-specific semantics, overload
resolution, dynamic lookup rules, metaprogramming, reflection semantics, module
loading, security policy, and foreign ABI details.

Minimum required feature set: `core, register_model, constants, arithmetic,
comparisons, control_flow, direct_calls, dynamic_calls, closures, exceptions,
type_checks, gc_ref_ops, profile_sites, ic_sites, deopt_metadata, osr_metadata`.
Everything else is capability-gated.

Final invariants: register-based; language-neutral; versioned; capability-based;
supports static and dynamic languages; supports direct/virtual/interface/prototype/
dynamic/closure dispatch; supports closures, exceptions, optional coroutines/async,
optional unsafe memory and FFI; supports profile sites, ICs, specialization with
fallback, precise deopt state, OSR, and GC root maps; supports runtime hooks and
extension opcodes; must be verifiable before execution; must be executable by all
four JIT tiers; must not require the JIT to know the guest source language.
