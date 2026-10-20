# Infra 4 — Advanced Threading & Suspension System (M6-b implementation contract)

The guest language needs a concurrency model, and the GC/JIT need to stop
threads safely. This document is the M6-b implementation contract: it replaces
the M0 stub description with the shipped semantics. Every section states the
observable contract the tests pin; deviations from the roadmap wording are
listed in section 9.

## 1. Scope and sequencing

M6-b ships the suspension substrate and the M:N scheduler:

- `SafepointPollingPage` — real reservation + arm/disarm + the global poll
  word all tiers read.
- `HandshakeManager` — the real request/acknowledge protocol with bounded
  waits (Rule 81).
- `WorkStealingPool` — M fibers on N OS workers via `ucontext` fibers with
  guard-page stacks and work-stealing deques.
- `GuardPageSuspender` — preemptive suspension of non-cooperative guest code
  through a stack guard-page trap.

Explicitly OUT of this batch (tracked in the roadmap M6 bullet): moving the
ICGGC driver slices to a background thread (the collector's M0 contract stays
one-mutator; `freeze_all` is the primitive that batch will consume), J3/J4
emitted poll sites (codegen work; the trap page is their consumer interface),
and lock-free Chase-Lev deques (v1 uses mutex-guarded deques — section 5.4).

## 2. Architecture: one poll word, three mechanisms

Every suspension mechanism converges on the same observable: the **global
poll word**, a `std::atomic<uint32_t>` living on the flag page.

- Zero == nobody requested anything (the common case; one relaxed load).
- Nonzero == a suspension of the encoded kind is requested; consumers take
  their slow path.

| Consumer | Mechanism | Slow-path behavior |
|----------|-----------|--------------------|
| T0 interpreter | relaxed load of the bound word at `SAFEPOINT_POLL` | invoke the hook (if set) or block until the word clears |
| J1 stencil | plain `u32` load of `ctx.safepoint_word` + `test` | exit `kErrSafepointRequest` (the armed word names its own error — the arming caller owns freeze/thaw choreography; a speculation deopt keeps the rerun-in-T0 contract, section 7) |
| J3/J4 (future) | load from the trap page | SIGSEGV trap → runtime suspension (interface reserved, not emitted this batch) |

The flag page stays readable in every state — cooperative consumers never
fault. The trap page is the one that flips protection.

## 3. SafepointPollingPage

### 3.1 Reservation layout

`reserve()` maps **one** anonymous region of `2 * page_size` bytes:

- Page A (offset 0): the flag page. Always `PROT_READ | PROT_WRITE`.
  - `+0` `u32` the global poll word (`std::atomic<uint32_t>`, value 0).
  - `+4` `u8` armed kind (`SuspensionKind`, 0 = none).
  - `+8` `u64` arm generation (increments on every arm; observability).
- Page B (offset page_size): the trap page. Readable when disarmed, flipped
  to `PROT_NONE` when armed. Holds no data; its address is the load target
  for future emitted polls.

`reserve()` is idempotent: the first call creates the process-wide instance;
later calls return the same base. A static member returns the base without
allocating after the first reserve.

### 3.2 Arm/disarm state machine

- `arm(kind)`:
  1. `mprotect(pageB, PROT_NONE)`.
  2. Store kind + bump generation (release).
  3. Store the poll word = `0x80000000 | (kind << 1) | 1` (release).
  Ordering: the trap page is hostile BEFORE the word turns nonzero — a
  cooperative consumer that observes the armed word can rely on the trap
  page already being armed (consumers that never load from the trap page
  are unaffected).
  Errors: `mprotect` failure → named reject (`PollingPageProtectFailed`),
  state unchanged, the word stays 0.
- `disarm()`:
  1. Store the poll word = 0 (release).
  2. `mprotect(pageB, PROT_READ | PROT_WRITE)`.
  Ordering: the word clears BEFORE the trap page becomes readable — a
  consumer unblocked by the cleared word never races the flip.
  Errors: named reject (`PollingPageUnprotectFailed`); the word is already
  0, so cooperative consumers are safe either way; the instance is marked
  degraded and `arm` rejects with `PollingPageDegraded` afterward.
- Both are idempotent (arm while armed re-arms with the same protocol;
  disarm while disarmed is a no-op success). Not thread-serial: a mutex
  serializes arm/disarm against each other; the word store itself is
  atomic, so pollers never see a torn value.
- `poll_word()` returns `std::atomic<uint32_t>*`; `state()` returns
  `{armed, kind, generation}` for tests/observability.

### 3.3 Non-atomic reads from generated code

The J1 stencil reads the word with a plain `mov` (hand-written asm, not
C++), so the C++ abstract-machine UB question does not arise; the C++ side
always accesses it through `std::atomic<uint32_t>`. The encoder must keep
`std::atomic<uint32_t>` lock-free on x86-64 (static_assert in the header,
`include/vortex/infra/threading.hpp`).

## 4. HandshakeManager

### 4.1 Thread table

Fixed-capacity table (`kMaxHandshakeThreads = 4096`, Rule 28: bounded).
`register_thread()` claims a slot and returns the `uint64_t` id (1-based;
0 is the legacy wildcard that maps to "the calling thread if registered,
otherwise a valid anonymous slot" — the LDPT consumer passes 0 today and
must keep working: an unregistered id 0 request records against an internal
anonymous slot so the request/patch/acknowledge flow stays observable).

Per-slot state (atomics only; no locks on the consumer side):

- `pending` u32: 0 none, else `SuspensionKind + 1`.
- `acked` u32: set by the owning thread's `acknowledge`, cleared by
  `release`.
- `alive` u32: registered / exited.

### 4.2 Protocol

- `request(kind, tid)` — post only; NEVER blocks (the LDPT flow does
  request → patch → acknowledge on one thread and must not deadlock).
  Sets `pending` for one id, or every alive slot for `kAllHandshakeThreads`
  (0xFFFF'FFFF). Named rejects: unknown id, unregistered id (except the
  anonymous-0 case). The request counter counts ACCEPTED requests only
  (Rule 120: rejects must not inflate telemetry).
- `acknowledge(tid)` — the (own or foreign) thread records it reached the
  safepoint: clears `pending` for that id and sets `acked`. Waiting callers
  wake via the manager's condvar.
- `wait_acknowledged(tid, timeout)` — bounded block until the slot's
  `acked` is set. Timeout (`std::chrono::nanoseconds`, default 10 s) →
  named reject `HandshakeTimeout` (Rule 81: bounded latency; a stuck thread
  fails the freeze loudly, never forever).
- `release(tid)` — clears `acked` + `pending`; the thread resumes its
  cooperative poll loop (the poll word was disarmed by the freezer before
  release in the GC-freeze choreography).
- `freeze_all(kind, timeout)` — the STW primitive. The frozen flag is a
  CAS gate: the AlreadyFrozen check and the publication are one atomic
  transition, so two concurrent freeze_all calls can never both pass.
  Then: arm the polling page, `request(kind, kAllHandshakeThreads)`, wait
  for every alive registered slot under ONE shared deadline (the worst
  case is the caller's budget, never N × budget). A thread that EXITS
  during the window satisfies the freeze (it can no longer mutate).
  Registration (`register_thread`) and pool spawns are rejected for the
  whole window — a late registrant would run as a mutator inside a
  "stopped" world. On timeout the freeze rolls back (thaw) and rejects
  by name. Returns the freeze generation as an observability token; v1
  does NOT gate `thaw_all` on it (any subsystem may end a freeze —
  acceptable for the single-driver v1, revisited with the GC batch).
  `thaw_all()` — disarm + `release(kAllHandshakeThreads)`.
- Debug dumps (the handshake wait timeout path) are gated behind the
  `VORTEX_THREADING_DEBUG` environment variable; the trap handler itself
  does no I/O (async-signal-safety — see section 6.2).
- `pending(tid)` keeps its stub meaning (any pending request for the id).

Deadlock honesty: `wait_acknowledged` only succeeds for threads that
actually reach a poll. It is the caller's contract to request only threads
guaranteed to poll within the budget — or to use the guard-page path
(section 6) for threads that may not. The header says this in CEP:ASSUMES.

## 5. WorkStealingPool (M:N scheduler)

### 5.1 Fibers

`FiberContext` (fixed-size struct, no heap per yield):

- `ucontext_t ctx` — the fiber's resumed state.
- stack: one anonymous mmap of `stack_size + page_size`; the LOW page is
  the guard page (`PROT_NONE` for the fiber's lifetime, owned by
  `GuardPageSuspender`'s registry); guest stack grows down above it.
- `uint64_t id`, `std::atomic<uint32_t> state` (Ready/Running/Suspended/
  Done), entry fn + user arg, park point (`sigjmp_buf`), captured trap
  context (`ucontext_t` + valid flag).

`makecontext` trampolines through a static thunk (the fiber id carried as
two int halves, LP64-safe; the pool and fiber resolved through the
thread-current scheduler slots). Entry returns → state Done → worker
continues its loop.
Exceptions must NOT cross the fiber boundary (Rule 114: suspension points
are semantic boundaries); guest-facing failures propagate as `Result`
values through the entry's own signature. The pool's worker loop catches
nothing and needs no unwinding — entries are `Result<void>(void*)`
functions whose unexpected values are recorded on the fiber and surfaced
by `join`.

### 5.2 Workers

`WorkStealingPool(n)` starts `n` pthread workers (n >= 1). `n == 0` is
DRIVER-ONLY mode: a VALID pool with no worker threads — fibers are run by
the embedder through `resume_fiber` (the deterministic path the DoD's
preemptive tests drive; the suspension fault then fires on the caller's
thread with the caller's own trap disposition). Each worker: loop { pop
local; if empty, steal (random victim, bounded backoff spin then condvar
wait); if a fiber is popped, run it (`swapcontext` into the fiber); else
park }. A failed worker start leaves a degraded pool that rejects spawn
by name.

Scheduler synchronization (review-hardened): spawn pushes to a worker's
deque under THAT worker's mutex (workers pop the front, stealers pop the
back — a push under any other lock races them). A fiber is claimed for
running by a CAS from Ready-or-Suspended to Running: two threads can
never run one fiber; a stale queue entry loses the CAS and is counted in
`stats().run_rejected`. A yield requeues the fiber only AFTER its context
is fully saved (the requeue runs in the scheduler after the swapcontext
returns — requeueing before the save let a stealer swap into a stale
context while the first activation was still live).

### 5.3 Scheduling API

- `spawn(entry, user, stack_size)` → fiber id (or the `GreenThread&`
  overload: id/stack_base/state are filled on success — the struct is the
  out-param channel). stack_size defaults to 256 KiB, minimum 64 KiB
  (named reject below — the guard page plus one frame must fit). Fibers
  vastly outnumber workers (the M of M:N); the table is a bounded deque
  of ready fibers plus a bounded map of live fibers (kMaxFibers = 65536,
  Rule 28). Spawning during a freeze window rejects by name.
- `yield_current()` — cooperative: the RUNNING fiber on the calling OS
  thread swapcontexts back to its worker loop (the requeue happens in the
  scheduler after the save — see 5.2). Calling from a non-fiber thread →
  named reject `NotFiberContext`.
- `suspend_fiber(id)` — arm the preemptive patch (section 6.3);
- `resume_fiber(id)` / `fiber_state(id)` — run a fiber until its next
  yield/done/trap from ANY thread (workers and embedders share the
  claim-CAS), and observe its scheduler state (Rule 120).
- `join(id)` — block the CALLING FIBER (or OS thread) until the fiber is
  Done; returns the entry's `Result<void>` (the recorded unexpected). A
  fiber whose stack overflows into its lifetime low guard is failed by
  name — join propagates the overflow diagnostic (section 6.3).
- `shutdown()` — drain: stop accepting spawns, run every queued fiber to
  completion, join workers. Idempotent. Suspended fibers reject the drain
  by name (`ShutdownWithSuspendedFibers`); the destructor fails them
  explicitly before reclaiming (no silent bypass).
- `stats()` — spawned/completed/stolen/yields/freeze_polls/run_rejected
  counts (observability, Rule 120).

### 5.4 Deques and stealing (v1 deviation)

Per-worker FIFO deque guarded by a mutex; steal pops from the victim's
tail. This is deliberately boring: correctness first, the lock-free
Chase-Lev upgrade is tracked in the roadmap. Steal attempts pick random
victims (xorshift seeded per worker), bounded to `n` tries per idle round.

### 5.5 Safepoint integration

`pool.bind_suspension(&handshake, &polling_page)` wires both into the
pool and REGISTERS every existing fiber with the handshake; every later
spawn registers too (a full table leaves the fiber unregistered — it
never acknowledges and the freeze does not wait on it; documented
degrade). When a bound fiber is done, its slot is released for reuse.
`adopt_interpreter(Interpreter*)` installs, into every Interpreter the
pool is told about, the T0 hook: on a fired poll — acknowledge the
handshake (for the running fiber's registered slot), block while the poll
word is nonzero (cooperative freeze, section 4.2 choreography), and yield
the fiber every `quantum` polls (default 1024) so M:N interleaving
actually rotates. Note: a hook installed while no polling page is
reserved never fires — the interpreter's effective word is null and the
poll stays a counted no-op (the documented binding contract).
Not-adopted interpreters keep whatever hook their owner set (the pool
never steals a hook back).

## 6. GuardPageSuspender (preemptive suspension)

### 6.1 Arm/disarm

- `arm(region_base, size, record)` — page-align down the base, page-align
  up the end, publish AND activate the registry entry (base/bytes/record +
  the active flag), THEN `mprotect(PROT_NONE)` (the publish-before-protect
  order: a descent crossing the page inside the protect window must find
  its record — any later publication leaves a window where the suspension
  fault classifies as foreign; no fault can consume the entry while the
  page is still readable, and arms are serialized by the registry mutex).
  On mprotect failure the entry rolls back — state unchanged. Registry:
  mutex, fixed capacity 1024, Rule 28. `record` is an opaque per-target
  token (the suspender stores it verbatim; the pool passes the fiber's
  park record). Named rejects: zero size, region overlaps an armed
  region, registry full, mprotect failure.
- `disarm(region_base)` / `disarm_record(record)` —
  `mprotect(PROT_READ|PROT_WRITE)` + deactivate, by exact base or by the
  owning record (a fiber's lifetime guard and its suspension patch share
  the record; teardown reclaims BOTH — the low guard by base, then any
  remaining patch for the record).
  The guard pages on fiber stacks are armed at fiber creation (the pool
  owns that lifecycle; `disarm` there only ever runs at fiber teardown).
  Suspension use: arm a SECOND region near the running fiber's current SP
  (the deep-recursion backstop) — `arm_sp_patch` arms the single page just
  below a supplied SP; the next guest push faults.

### 6.2 Trap handling

`enable_trap_suspension()` installs one `SIGSEGV` handler
(`SA_SIGINFO | SA_ONSTACK`) over runtime-owned PER-THREAD altstacks
(64 KiB each — see 6.3). Handler algorithm (async-signal-safe ops only:
atomics, memcpy, `siglongjmp` — no stdio, no malloc, no locks):

1. Fault address in a registered guard region?
   - No → chain to the previous disposition WITHOUT uninstalling: a
     SA_SIGINFO predecessor is CALLED DIRECTLY (sanitizer reports and
     genuine bugs keep their normal semantics; restoring-and-returning
     would permanently lose the runtime the first time a previous handler
     returns). Only a default disposition restores (the re-executed
     instruction refaults into it and dies). This chaining is what keeps
     ASan/UBSan runs meaningful with the runtime armed. The handler does
     no I/O on this path — the sanitizer report IS the diagnostic.
   - Yes, and the region is the record's LIFETIME base (the fiber's low
     overflow guard) → mark the record `kStateFaulted` and longjmp: the
     fiber FAILS by name ("fiber stack overflow"). Suspending here would
     let resume disarm the lifetime guard and re-execute the overflowing
     access on a readable page — silent corruption below the mapping.
   - Yes, otherwise (a suspension patch) → copy `ucontext_t` (the
     handler's third argument) into the record's slot, mark the record
     suspended, `siglongjmp` to the record's park point with value 1.
2. The park point is `sigsetjmp(..., 1)` taken by the suspender's own
   probe wrapper immediately before entering the suspension window (the
   pool parks each fiber there before running it).

`disable_trap_suspension()` restores the previous handler (idempotent,
named rejects if never enabled). The per-thread altstacks live for the
thread's lifetime (they are thread_local; no free-then-race window).

### 6.3 Trap delivery and resume choreography

The disposition is process-wide; the ALTSTACK IS PER-THREAD state
(sigaltstack is per-task) — every thread that can take a guard trap
installs its own 64 KiB altstack (`ensure_thread_altstack`, called by
enable() for the caller and by every pool worker at startup). 64 KiB
because sanitizer builds run fat interceptor frames for TLS access inside
the handler; SIGSTKSZ*2 overflows them.

Foreign faults chain to the previous disposition WITHOUT uninstalling:
if the previous handler is SA_SIGINFO, it is CALLED DIRECTLY (sanitizer
reports keep working; the runtime stays installed — restoring-and-
returning would permanently lose the runtime the first time a previous
handler returns). Only a default disposition uninstalls (the re-executed
instruction refaults and dies). Host runtimes that re-install their own
disposition when a thread starts are handled by `ensure_installed()`
(called by pool workers; re-asserts ours, demoting the displaced handler
to the chain target).

The capture copies the FP state out of the signal frame bounded by the
altstack end (the frame sits at the TOP of the altstack; fpregs + 4 KiB
can cross it), unblocks SIGSEGV in the saved mask, and clears uc_link.
Resume disarms EXACTLY the region that trapped (the fiber's lifetime low
guard stays armed) and swapcontexts into the captured context — the
faulting access re-executes on a readable page. `sigsetjmp/siglongjmp`
discipline: locals read after the jump are volatile (the worker's trap
classification would otherwise be indeterminate).

`WorkStealingPool::suspend_fiber(fid)` bases the patch on the fiber's
LAST-PARK RSP — the entry trampoline's frame for a never-started fiber,
RE-BASED AT EVERY YIELD by the scheduler (the saved context's RSP is the
fiber's live depth at the park; a fiber that descended deeper than its
entry frame still gets a patch just below its real stack — the review
round's M10 fix). Always inside the fiber's own mapping and above every
descent deeper than the last park. A never-started fiber uses the
primed RSP directly: the page below it is the LAST usable page, which
the entry's first frames occupy.

Latency: the trap fires when the descent next crosses the armed page —
bounded by one frame push past the last-park basis (a fiber that looped
permanently BELOW its last park without crossing the patch needs the
cooperative poll path — the documented scope). The DoD tests assert
suspension within a wall-clock budget for BOTH bases: the never-started
fiber and the started-deep fiber (Rule 81).

### 6.4 What preemptive suspension means for this batch

The suspender is scheduler-agnostic: it needs a park point and a record
token, not the pool. The pool wires them (fibers park in the worker loop).
A plain OS thread can also use the suspender (its park point set by its
owner) — the DoD covers the fiber path; the bare-thread path is the same
mechanism with `enable` + `arm_sp_patch` + park.

## 7. Integration contracts (existing consumers)

- **T0** (`src/vm/interpreter.cpp`): `SAFEPOINT_POLL` becomes flag-first:
  relaxed load of the bound word; zero → next opcode. Nonzero → the slow
  path: the installed hook if any, else block while the word is nonzero
  (re-check loop). `bind_safepoint_word(std::atomic<uint32_t>*)` defaults
  to the global poll word. The hook pointer remains for scheduler
  integration (section 5.5) — but it is no longer ON the unguarded path,
  so **PERF-003 is retired**: the annotation block comes out of
  interpreter.cpp and the register line comes out of compliance.sh.
  `stats_.safepoint_polls` still counts every executed poll.
- **J1** (`src/j1/baseline_jit.cpp`): `bindings.safepoint_word` defaults
  to the global poll word (raw u32 view of the atomic); the per-executable
  override remains (the fallback contract is pinned by its own test).
  Armed word → `kErrSafepointRequest` exit — the M6-b review split: the
  poll no longer rides the deopt id, so a speculation deopt that races an
  armed word keeps the rerun-in-T0 contract while the armed word names
  its own error and the arming caller owns freeze/thaw choreography
  (rerunning T0 on a still-armed word would self-deadlock on its
  no-hook poll). No C++ code reads the word through the raw view —
  C++ readers use the atomic view or `std::atomic_ref`.
- **LDPT** (`src/runtime/ldpt.cpp`): call shape unchanged
  (`request(Gc, 0)` → patch → `acknowledge(0)`); semantics upgrade from
  stub-record to real pending flags, with the anonymous-0 slot behavior of
  section 4.1. Existing LDPT tests keep passing unmodified.
- **GC** (out of scope this batch): `freeze_all`/`thaw_all` is the STW
  primitive; the collector's one-mutator M0 contract is untouched.

## 8. Platform

Linux x86-64 only (mmap/mprotect/ucontext/sigaction/SIGSTKSZ). The .cpp
defines feature-test macros BEFORE any system include (never in headers).
Any other platform: every entry point returns the named
`UnsupportedPlatform` reject (Rule 29: degrade, never crash) — no partial
implementations. `static_assert(sizeof(void*) == 8)`.

## 9. Rule compliance and deviations

- Rule 81: safepoints exist at T0 polls (backedges/calls carry them via
  UGB semantics), latency bounded by handshake timeouts AND the guard-page
  preemptive path for non-cooperative code. DoD pins both budgets.
- Rule 9: every failure is a named `Result` reject; no `.at()`, no throw
  paths (Rule 65 interplay: the trap handler never throws; park/resume is
  `siglongjmp`/`setcontext`).
- Rule 28: thread table 4096, guard registry 1024, fiber table 65536,
  profile/telemetry counters bounded — all named rejects at capacity.
- Rule 114: exceptions never cross a suspension point; entries return
  `Result`.
- Rule 83: unchanged this batch — fibers do NOT concurrently allocate from
  one Heap (the one-mutator contract stands); multi-heap fibers are the
  GC-background batch's problem.
- Rule 120: stats() counters on pool + handshake (freeze counts, steals,
  timeouts).
- WorkStealingPool(0) is DRIVER-ONLY mode: a valid pool with no worker
  threads; fibers are run by the embedder through resume_fiber (the
  deterministic path the DoD's preemptive test drives — the suspension
  fault then fires on the caller's thread with the caller's own trap
  disposition).
- Deviations from the roadmap wording: "guard-page suspension" ships as
  fiber+thread preemptive suspension with chaining trap handling (the
  roadmap's "the next guest touch traps into the runtime" is exactly the
  section 6.2 algorithm); "millions of fibers" is bounded to 65536 live
  fibers in v1 (Rule 28; the roadmap's figure was aspirational prose).

## 10. DoD test list (tests/test_threading.cpp)

1. `polling_page_reserve_arm_disarm_cycle` — reserve idempotent, word
   states, trap-page protection flips (the honest probe: `read()` from
   `/dev/zero` INTO the trap page returns EFAULT when PROT_NONE — a
   `write()` to `/dev/null` never reads the source and would silently
   pass on a hostile page), re-arm generation bump. Degradation named
   errors (`PollingPageDegraded`) are specified but NOT injected in-suite
   (a failed `mprotect` cannot be provoked deterministically without
   fault injection hooks — tracked with the J3/J4 emitted-poll batch).
2. `polling_page_flag_visible_to_t0` — T0 method with a SAFEPOINT_POLL
   loop: disarmed word → runs to completion; armed word before run →
   poll takes the slow path (hook counter increments exactly once),
   method completes after the hook disarms.
3. `t0_safepoint_word_binding_default_and_override` — default binding is
   the global word; explicit `bind_safepoint_word` overrides (observable
   end to end through the bound word); nullptr resets to global.
4. `j1_global_poll_word_suspends` — J1-compiled loop suspends via the
   GLOBAL word (armed before exec → the named safepoint request).
4b. `j1_local_fallback_word_suspends` — the per-executable fallback
   binding behaves identically (the fallback contract, exercised
   explicitly).
5. `handshake_request_ack_roundtrip_cross_thread` — worker thread
   acknowledges; waiter wakes; release clears; anonymous-0 LDPT flow.
6. `handshake_wait_timeout_named_reject` — no acknowledger; bounded wait
   rejects by name; latency >= timeout; timeout counter.
7. `freeze_all_bounded_latency_with_polling_mutators` — registered
   mutator threads ack on the armed word; freeze_all completes within
   budget; second freeze rejects AlreadyFrozen; registration during the
   window rejects by name; thaw releases.
8. `workstealing_spawns_more_fibers_than_workers` — 32 fibers on 2
   workers; all complete; results correct; steal counter > 0 (imbalance
   forced by a 500 ms blocker window).
9. `workstealing_yield_rotates_execution` — fibers record interleaving
   via shared counters; quantum yields rotate; no fiber starves.
10. `workstealing_join_collects_entry_result` — failing entry's
    unexpected propagates through join; shutdown drains pending spawns;
    spawn after shutdown rejects; destructor shutdown is safe.
11. `guard_page_preemptive_suspension_bounded_latency` — a never-started
    fiber's basis is the primed RSP; suspension lands within budget; the
    recorded entry frame is OBSERVED inside the fiber's usable stack;
    resume finishes the suspended fiber with its loop state intact.
11b. `guard_page_rebased_basis_catches_started_deep_fiber` — the
    last-park basis: a fiber parked at a yield, then descending deeper
    than its entry frame in a no-poll loop, still suspends within budget
    (the review's M10 shape).
11c. `fiber_stack_overflow_fails_by_name` — unbounded recursion faults
    the lifetime low guard; the fiber is FAILED by name (never
    suspended); join propagates the overflow diagnostic; the guard stays
    armed to the end.
12. `guard_page_foreign_fault_chains_to_previous_handler` — a genuine
    wild deref under an enabled suspender still dies (default action) —
    asserted via a forked child (exit signal) so the suite stays alive;
    the disposition remains installed after the chain.
13. `suspender_arm_rejects_bad_regions` — zero size, null record,
    overlap (same base, sub-page offset, misaligned base), double
    disarm, AND the real 1024-region capacity bound (arm to exhaustion,
    reject by name, disarm restores).
14. `handshake_unknown_ids_named_rejects` — out-of-range ids, the
    permanent anonymous slot, register/unregister lifecycle with id
    reuse, AND the real 4095-slot table capacity (fill to exhaustion,
    reject by name, unregistering restores).
15. `freeze_over_polling_fibers_cooperative` — bound pool fibers
    REGISTER at spawn and acknowledge a real freeze from their poll
    loops (bounded latency, freeze_polls counter, held-at-poll observed,
    spawn during the window rejects, thaw completes them).

Implementation note for (1): protection is asserted with `read()` from
`/dev/zero` into the trap page (EFAULT when PROT_NONE, success when
readable) — no signals involved in the check. A `write()` of one byte
FROM the trap page to `/dev/null` is NOT a valid probe: /dev/null writes
never read the source buffer, so a hostile page would silently pass.
