# LDPT — Lazy-Devirtualized Patch Trampolines

LDPT is the mechanism that bridges static whole-program devirtualization
(massive compile-time cost) and adaptive IC dispatch (per-call indirect
branch overhead). When J4 compiles a speculative call site whose target
type is *probable but not guaranteed*, it emits a fixed-size **Skeleton
Trampoline** with a reserved patch hole; the first miss atomically
hot-patches the exact marshalling instructions at runtime.

Implementation: `include/vortex/runtime/ldpt.hpp`,
`src/runtime/ldpt.cpp`, `include/vortex/infra/patch_arena.hpp`. The M1
reference implementation executes real patched machine code under W^X
(tests/test_ldpt.cpp).

---

## 1. The Skeleton Trampoline

One carve per site inside a PatchArena (48-byte prologue + 16-byte hole +
256-byte out-of-line reservation):

```nasm
; --- SKELETON (emitted at J4 compile time) ---
mov  rax, imm64 PRIMARY_KLASS        ; imm64 patch site
cmp  rax, [rsi]                      ; receiver header.klass (offset 0)
jne  .hole                           ; always-routed hole until patched
mov  rdi, rsi                        ; receiver -> target ABI
mov  rax, imm64 PRIMARY_TARGET
call rax                             ; primary path: direct dispatch
ret
.hole:                               ; 16-byte reserved hole
mov  rdi, imm32 SITE_HANDLE          ; resolver ABI: rdi = handle
jmp  rel32 -> resolver_thunk         ; tail-jmp: one return address
nop; nop; nop; nop
; --- out-of-line reservation (escalation stubs rewrite in place) ---
```

Calling convention: the guest call site passes the **raw receiver in
rsi**; rdi is scratch (the hole loads the site handle). Escalation stubs
tail-jmp into their targets, so exactly one return address — the original
caller's — is ever on the stack.

Primary-path call form: the M1 arena is mmap'd and may lie outside
rel32 reach of the guest code range, so the primary call is
`mov rax, imm64; call rax` — a *target-constant* indirect that the BTB
predicts perfectly after first execution. Direct `call rel32` patching
returns with the M2 code-range reservation (the arena placed within ±2 GB
of published code, mirroring how production runtimes reserve their code
regions).

## 2. The Runtime Patching Protocol

Patching executable memory while other threads run it is the classic
tearing hazard. Vortex's M:N scheduler makes the protocol simple:

1. **The trap** — a miss tails into the per-arena resolver thunk
   (`mov r8,rdi; mov r9,rsi; mov rdi,imm64 mgr; ...; jmp rax`), which
   calls `LdptManager::resolve_miss(mgr, handle, receiver)`.
2. **The resolution** — the runtime target resolver (J4 dispatch tables
   in production; a host hook in M1) identifies the target for the new
   type. A lightweight J1/J2 stencil compile generates the marshalling
   sequence.
3. **The M:N safe-point freeze** — `HandshakeManager::request(Gc)` for
   all workers; virtual threads yield cooperatively at loop edges and
   calls (< 1 µs). M1 binds the handshake stub, which records the
   protocol ordering (request → patch → acknowledge); the real M2
   scheduler binds cooperative yields here.
4. **The W^X session** — `PatchArena::begin_session()` flips the arena
   RX→RW; the hole and OOL stub are patched; `end_session()` flips back.
   All writes of one escalation batch behind one flip pair
   (PERF-005, docs/cem26.md).
5. **Resume** — the resolver tail-jmps to the resolved target. The next
   hit on this site executes native code directly: no interpreter, no IC
   lookup, no C++.

The ordering IS the contract: no arena write may happen outside
request..acknowledge.

## 3. Escalation (mono → poly → mega)

The state machine mirrors the T0 IC machine (`ugb::IcState`,
docs/tier-t0.md section 3) with a distinct type: the IC tracks feedback,
the trampoline tracks native patch state.

- **Mono (1 type)** — the primary type dispatches through the prologue;
  the first miss assembles an OOL mono stub (klass imm64 guard + tail-jmp
  target) and rewrites the hole to `jmp rel32 stub`. The hole never
  changes again until Mega.
- **Poly (2–4 types)** — each new type rewrites the OOL stub in place as
  a compare chain (`kIcPolyCapacity == 4`, matching the IC's poly
  capacity). The hole is untouched.
- **Mega (5+ types)** — the hole becomes `mov rdi, handle; jmp rel32
  mega_thunk`. The mega thunk reads a **plain-RW data row** published by
  the manager: `mov rax, imm64(&row_slot); mov rax,[rax]; ...` —
  **updates are atomic pointer swizzles with a release store and require
  NO safe point and NO session** (docs/ldpt.md section 3 of the
  blueprint). Retired rows are kept alive so a reader holding a stale
  pointer stays correct; a row-lag miss degrades to the resolver.

The mega row scan is bounded (`kMegaRowCapacity = 16`); overflow degrades
the site to permanent resolver dispatch — correct, just not specialized.

## 4. Integration with Vortex Infrastructure

### A. Dependency invalidation & RBPD
`emit_skeleton` registers `AssumptionKind::MethodFinal` with the
DependencyGraph for (method_id, instruction_index). When a target module
unloads or is recompiled, `invalidate_method` yields an InvalidationBatch;
`invalidate_batch` rewrites the hole back to the resolver bytes and flips
the site to `TrampolineState::Resolver`. The trampoline **degrades** to
resolver dispatch instead of dangling. Lazy-batch entries need no patch:
the resolver path IS the lazy handler.

### B. ICGGC barrier injection
If the J4 graph could not prove generational isolation for a
cross-language store, the generated patch includes the barrier sequence
before the call — the same inline shape the J1 SET_FIELD stencil uses
(heap-base fold, `>> kCardShift`, byte store of DIRTY). Templates that
store references always emit it; pure-value marshalling omits it.

### C. W^X patch arenas
`PatchArena` (infra/patch_arena.hpp): RW during emission, one flip to RX
at publish, and **explicitly scoped sessions** for runtime patching. The
session API refuses nesting, refuses emission-after-publish, and refuses
patching outside a session — the W^X law is enforced by the type
interface, not by discipline. mprotect is the M1 backend (PERF-005);
MAP_JIT (Apple) / PKU slots behind the same session protocol.

## 5. Why this beats per-call IC dispatch

1. **No indirect-branch penalty on the steady path** — the patched path
   is a guarded direct/constant dispatch; V8's PICs pay an indirect
   branch per polymorphic call.
2. **Zero heap allocation for dispatch** — trampolines and stubs live in
   the code cache; the ICGGC never traces dispatch state.
3. **Cross-language at the UGB level** — the same mechanism devirtualizes
   a Python→Rust→JS chain at `CALL_VIRTUAL`/`CALL_INTERFACE` sites.
4. **M:N safe-point patching** — multi-byte instruction sequences are
   patched while no thread executes them; no atomic CAS loops over live
   instruction bytes.

## 6. The compile/runtime contract

| Phase | Action | Latency |
| :--- | :--- | :--- |
| Compile (J4) | Emit skeleton + 16-byte hole; resolve primary type | Bounded (no permutation explosion) |
| First miss | Codegen 3–4 instructions; safe-point; session; patch | ~2–5 µs, once per (site, type) |
| Steady state | Direct execution of patched instructions | Zero added overhead |

CEM-26 cost model: the trampoline fast path is machine code (its budget
is the guard compare + dispatch); the C++ resolver is @cold (PERF-007);
the patch session is @cold with the registered mprotect permit (PERF-005).
The C++ dispatch mirrors (`TrampolineSite::lookup`, `MegaRow::lookup`)
carry the @hot contracts for future C++-side fallbacks.

## 7. Review notes

- The resolver is `noexcept` by contract; OOM during escalation
  terminates deliberately (an allocator failure inside a patch protocol
  is not recoverable in-place).
- Arena layout is frozen at publish; escalation stubs rewrite within
  their per-site reservation (`kOolReservationBytes = 256`, pinned by
  static_assert against the worst-case poly-4 chain and the mega thunk).
- Site records are escalation metadata, not per-call data
  (`sizeof(TrampolineSite) <= 192` pinned).
