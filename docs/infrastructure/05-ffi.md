# Infra 5 — Native Interop (FFI) & ABI Translation System

Guest code must call C/C++/Rust, and native code must call back into the JIT.
The M6-c batch lands the two contract-stubbed mechanisms for real: the
**trampoline generator** (synthesized SysV x86-64 stubs through the one W^X
publish path, assembled with the M1 assembler) and the **unified stack
unwinder** (mixed JIT/trampoline/native frame walk over the published-code
registry, plus the named-failure exception bridge).

## Mechanisms

- **Trampoline generator.** Dynamically generates machine-code stubs that
  translate the UGB calling convention (virtual registers, tagged values) to
  the native OS ABI (System V / ARM AAPCS) and back.
- **Object pinning & handle tables.** Because the ICGGC moves objects
  concurrently, native code cannot hold raw pointers. The FFI system provides a
  **handle table** (indirection pointers) or **pinning regions** (temporarily
  forbidding the GC from moving specific objects during the native call).
- **Unified stack unwinding.** One RBP-chain walker names every frame of a
  mixed stack — JIT frames (through the published-code registry,
  docs/gc-icggc.md 8.6), trampoline frames (through the FFI registry), and
  native frames (through `dladdr` symbolization). It consumes no
  `.eh_frame`/DWARF data in this batch: the chain is the contract, and the
  build keeps native frame pointers in every target. C++ exceptions never cross a
  boundary frame: they are translated to named failures at the boundary
  (Rule 132: every exception carries a rule id; the exception register tracks
  the boundary semantics).

## Tagged-value encoding facts (normative for the emitters)

The stubs are machine code; they hard-code the TaggedValue layout
(include/vortex/support/tagged_value.hpp — static_assert-pinned):

- Smi: payload shifted left by 1, bit0 == 0. Valid range is int63
  `[-(2^62), 2^62 - 1]` (`smi_min`/`smi_max`). Box = bounds compare + shift;
  unbox = arithmetic shift right by 1.
- Canonical non-Smi words: null = 0x3, undefined = 0x7, false = 0xB, true =
  0xF, heap pointers carry tag nibble 0b0001 over a 16-byte-aligned address.
- Boxed doubles: `ObjectHeader` (24 bytes, static_assert-pinned) + 8-byte
  IEEE binary64 payload at offset 24 (`read_boxed_double`).

## Trampoline calling conventions (normative)

### Guest → native (`emit_guest_to_native`)

The synthesized stub is entered as:

```
TaggedValue stub(const TaggedValue* args, TrampolineContext* ctx)
// SysV: RDI = args pointer, RSI = ctx pointer
```

Machine-code behavior, in order:

1. Frame: push RBP / mov RBP,RSP; callee-saved RBX and R12 are spilled
   (R12 holds the tagged args array, RBX the context). Trampoline frames
   are RBP-chained and carry a type tag at [rbp-8] (`VFXF0123`) for future
   secondary tooling; M6-c classification is pc-range-only (the registry).
2. Boundary safepoint BEFORE argument translation: the stub loads the
   global poll word (`SafepointPollingPage::poll_word_raw()` — through the
   context's poll_word field) once; if armed it calls the
   `vortex_ffi_safepoint_boundary` helper before anything else. The hook is
   a normal C call, so it MUST run while the only live values are the
   callee-saved ones (the hook clobbers every caller-saved register —
   translating first would destroy the translated arguments). Foreign
   frames never poll (they are opaque to the runtime), so the boundary is
   where the mutator acknowledges pauses, and the mutator never sits
   between the poll and the foreign frame. The GC contract: a collection
   started while foreign code runs waits at the nearest boundary, bounded by
   the foreign call duration — foreign callees must be bounded or must
   re-enter through callbacks that poll.
3. Argument translation: for signature argument *i*, load the tagged word
   from `[RDI + 8*i]`:
   - `Int` — assert Smi (bit0 test); `sar $1` into the next SysV integer
     register (RDI, RSI, RDX, RCX, R8, R9). A non-Smi word at an Int
     argument is a boundary type error (slow-path helper, named failure).
     Signatures exceeding 6 integer or 6 F64 arguments are named
     emit-time refusals in this batch (native stack-arg spill lowering
     ships with the AAPCS64/stack-arg batch — refusing is deterministic
     and diagnosable; half-lowering is neither).
   - `F64` — the guest passes a boxed double; the stub validates the tag
     nibble, loads the payload from `[obj + 24]` into the next XMM register
     in sequence (XMM0–XMM7; SysV classifies double args as SSE).
   - `Ptr` — the tagged word must be pointer-like (heap tag or null); the
     unboxed value is the 16-byte-aligned object address (heap invariant).
     Native code must NOT dereference it across a safepoint — raw pointers
     cross only inside a `PinningRegion` or as handles (section below).
4. Call: the foreign function pointer comes from the `TrampolineContext`
   (loaded into R10/RAX, then `call`).
5. Return translation per `return_kind`:
   - `Int` — box RAX: bounds compare against int63, in-range shifts left by
     1; out-of-range calls the slow-path helper `vortex_ffi_box_smi_slow`,
     which records the overflow in the context and returns the canonical
     boundary-error word (undefined). The stub returns that word.
   - `Bool` — the native 0/1 return normalized to the canonical true/false
     words.
   - `Void` — canonical undefined word.
   - `F64` — not expressible in this batch: `ForeignReturnKind` carries no
     F64 member (boxing a double requires a heap allocation; the
     allocation-integrated form ships with the emitted-poll/J4 batch). The
     refusal is structural — compile-time, not a runtime surprise.

The stub is emitted into a `WritableCodeMemory` (RW phase → publish flip);
after publication the bytes are immutable like every other published code.

### Native → guest (`emit_native_to_guest`)

The synthesized stub IS a SysV function handed to native code:

```
uint64_t stub(uint64_t a0, ..., uint64_t a5)   // integer args
// double args arrive in XMM0–XMM7 per SysV classification
```

Machine-code behavior, in order:

1. Frame as above (RBP-chained, typed frame).
2. Argument translation: each signature argument is boxed into a
   stack-resident tagged buffer:
   - `Int` — range check (int63), shift left by 1; out-of-range calls the
     slow-path helper which records the error in the callback context and
     substitutes null.
   - `Bool` — canonical true/false words.
   - `Null` — canonical null word (for guest signatures with nullable
     positionals).
   - `F64` / `Ptr` — **refused at emit time** in this batch (both require
     heap allocation or pin discipline the callback context does not have
     yet; named emit error).
3. Shim call: the context's C++ entrypoint is invoked as

```
extern "C" uint64_t vortex_ffi_callback_shim(
    const TaggedValue* args, size_t nargs, void* user)
```

   with RDI = tagged buffer, RSI = arity, RDX = the context's user pointer.
   The shim (provided by the embedder) re-enters the engine — typically
   `Interpreter::run` at T0 or a published tier entry — and returns the
   tagged result word (uint64 bit pattern of the TaggedValue).
4. Return translation per `return_kind`: `Int` unboxes (Smi test, `sar $1`;
   a non-Smi return records a boundary type error in the context and yields
   0), `Bool` yields 0/1 from the canonical words, `Void` yields 0.

### Deterministic compile

Stub emission is a pure function of (signature, context pointers): the
same inputs produce byte-identical CODE bytes, with one named exception —
the callback stub's rip-relative island CONTENT is the heap context's
address (necessarily allocation-dependent; the island DISPLACEMENT is
layout-only and deterministic). Golden-byte pins in tests/test_ffi.cpp lock
the encodings (the M1 golden-pin precedent; the callback pin covers the
first 48 code bytes, all island-free); no entropy, no ASLR-derived CODE
bytes, no addresses baked into code (foreign fn / shim / poll word ride
context fields; the slow-path helpers ride context slots).

## Unified unwinder

`UnifiedUnwinder::walk(rbp, pc, max_frames)` starts from a captured
(RBP, return-pc) pair and walks the RBP chain, classifying each frame:

1. **JIT frame** — the return pc falls inside a record of the
   `GcCodeRegistry` (the M5 published-code registry): the frame reports its
   pc offset within the published range (the record itself carries no method
   id; consumers resolve the record through the registry when they need
   more). GC maps are the collector's concern (already consumed by
   stack_scan); the unwinder only names frames.
2. **Trampoline frame** — the return pc falls inside a published FFI
   trampoline (the `FfiTrampolineRegistry`, populated by every
   `TrampolineGenerator` emit). Trampolines keep RBP-chained frames and hold
   no tagged live state across the foreign call (arguments are translated
   into registers/stack before the safepoint), so they need no GC maps —
   only identification.
3. **Native frame** — everything else: `dladdr` resolves the nearest exported
   symbol (falling back to the shared-object name); when `dladdr` fails the
   frame reports a null origin pointer.

The walk terminates on: a null pc, a null RBP, a non-monotonic RBP move,
a step larger than 8 MiB (stack adjacency — a frameless function's garbage
saved-rbp that looks monotonic is rejected unless it lands within one
plausible frame step of the current RBP), or `max_frames`. The plausibility
bounds make following garbage UNLIKELY, not impossible: a corrupt chain is
already undefined behavior, and the walker is a diagnostics-path API (the
M6-b guard-page work owns the preemptive case for frames that violate the
chain at runtime).

### Exception bridge (Rule 132)

`ffi_boundary_call` invokes a `Result`-returning callable under the
boundary's try/catch and translates ANY escaping C++ exception into a named
`Result` failure (`ErrorCode::ForeignBoundary`, message naming the boundary
and the exception's `what()`). Direction rules:

- Foreign → guest: an exception thrown inside foreign code that would
  otherwise unwind through guest frames is CAUGHT at the guest→native
  trampoline's shim and translated — guest frames never observe foreign
  exception objects.
- Guest → foreign (callback): a guest trap (a `Result` error from the shim's
  engine re-entry) is recorded in the callback context and the stub returns
  the canonical error encoding; native callers inspect the context. Guest
  traps are never thrown as C++ exceptions across the boundary.

Rationale: the engine's exception register (docs/exception-register.md)
tracks every exception path as a rule-bearing, named event; silent cross-stack
unwinding would bypass that register and the deopt state machines.

## Vortex API

`include/vortex/infra/ffi.hpp`:

- `AbiKind` (SystemV x86-64, AAPCS64), `ForeignSignature` descriptors
  (ordered per-argument kinds, return kind, derived SysV classification).
- `TrampolineGenerator` — UGB <-> native ABI stub synthesis contract
  (M6-c: real SysV x86-64 emission through WritableCodeMemory; AAPCS64 is
  the Post-M6 ARM64 backend item).
- `FfiTrampolineRegistry` — process-wide registry of published trampoline
  ranges, consumed by the unified unwinder (and future diagnostics).
- `HandleTable` — scope-checked global/weak/local handles (implemented;
  unit tested with allocation/free scopes).
- `PinningRegion` — RAII pin scopes the GC consults.
- `UnifiedUnwinder` — mixed-stack walker (JIT / trampoline / native) +
  `ffi_boundary_call` exception bridge.

## Definition of done (M6-c, verifiable)

1. Golden machine-code pins: the guest→native stub for a fixed signature and
   the native→guest stub for a fixed signature assemble to pinned byte
   sequences (first 48 code bytes each, incl. the frame prologue and the
   poll load; the callback island content is the named exception),
   byte-identical across repeated emissions (determinism DoD).
2. Parity: a guest→native stub calling an `int64 add(a,b)` foreign function
   returns the boxed sum for smi-representable inputs; a non-Smi tagged word
   at an Int argument records a boundary type error; an out-of-int63-range
   return records an overflow — both via the context, stub still returns.
3. F64 arguments: a boxed-double argument arrives at the foreign function as
   the exact payload bits (bit-identical through xmm), including a negative
   and a subnormal value.
4. Callback parity: a native→guest stub re-enters the engine through the
   shim (T0 `run` of a Mini add function) and returns the same result the
   engine reports directly (boxed and unboxed forms agree).
5. Boundary safepoint: with the global poll word armed, the guest→native
   stub invokes the safepoint boundary helper exactly once before the
   foreign call; disarmed, zero invocations.
6. Mixed walk: a stack sampled inside a foreign call made from a real J1
   published frame walks as [JIT frame, trampoline frame, native frames...],
   with the JIT frame's pc offset inside the J1 record's published range and
   the trampoline frame identified by the FFI registry.
7. Exception bridge: a foreign callable that throws produces a named
   `ForeignBoundary` failure carrying the exception text; nothing escapes
   the boundary. A callback shim returning a non-Smi for an Int signature
   records the type error in the callback context and yields 0.
8. Emission refusals: F64/Ptr callback arguments, Null call-out arguments,
   and signatures exceeding 6 integer or 6 F64 arguments are named
   emit-time errors with no side effects (the F64 RETURN refusal is
   structural: ForeignReturnKind cannot express it — compile-time
   refusal). The build keeps native frame pointers in every mode
   (-fno-omit-frame-pointer) — the RBP-chain contract of the walker.
9. The full suite stays green on Release and ASan/UBSan (zero sanitizer
   findings); compliance.sh passes; the ffi tree + test_ffi.cpp ship
   CEP&CC-complete (SEV1-clean, CEP blocks on the hot stub paths).
