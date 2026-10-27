// CEP:FILE: include/vortex/infra/threading.hpp
// CEP:WHAT: Infra 4 — advanced threading & suspension (M6-b, docs/
//           infrastructure/04-threading-suspension.md): the safepoint
//           polling page (global poll word + protection-flipped trap page),
//           the real HandshakeManager request/acknowledge protocol with
//           bounded waits, the M:N work-stealing fiber scheduler
//           (ucontext fibers on mmap'd guard-page stacks), and the
//           GuardPageSuspender (preemptive suspension through a stack
//           guard-page trap with chaining signal semantics).
// CEP:WHY: The guest language needs a concurrency model and the GC/JIT need
//           to stop threads safely (Rule 81: safepoints reachable within
//           bounded time). T0 already executes cooperative polls (M0), J1
//           templates already test a global poll word (M1), and the LDPT
//           patch protocol already drives request/acknowledge (M2) — this
//           module is the real substrate all three consumers share.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Every failure is a named Result reject ("threading: <what>").
//           Handshake waits are bounded (HandshakeTimeout) — a stuck thread
//           fails the freeze loudly, never forever (Rule 81). Resource
//           exhaustion (thread table, guard registry, fiber table) rejects
//           with named errors; nothing degrades silently (Rule 9, Rule 28).
// CEP:ASSUMES: Linux x86-64 (mmap/mprotect/ucontext/sigaction); other
//           platforms get UnsupportedPlatform rejects from every entry
//           point (Rule 29: degrade, never crash). The poll word is
//           accessed by C++ through std::atomic<uint32_t> and by hand
//           written J1 asm as a plain u32 (asm is not C++ — no abstract
//           machine UB; C++ writers always use release stores, readers
//           acquire/relaxed). Guard-page capture/resume uses the signal
//           handler's ucontext with the FP state copied out of the signal
//           frame (4096-byte bound, x86-64 xsave area). Exceptions never
//           cross a fiber boundary (Rule 114): entries return Result.
// CEP:COST: @cold for setup (reserve/arm/enable). The poll fast path is one
//           relaxed atomic load per SAFEPOINT_POLL. Scheduler operations
//           are O(queues) under small mutexes (v1 deques — lock-free
//           upgrade tracked in the roadmap). Guard registry lookup is a
//           bounded linear scan (1024 regions) executed only in the trap
//           handler.
// CEP:EVIDENCE: tests/test_threading.cpp — polling-page state machine +
//           protection probe, T0 flag-first polls (hook + count), J1 global
//           word suspension parity, cross-thread handshake round trip +
//           timeout, freeze_all bounded latency over polling fibers, M:N
//           spawn/steal/yield/join/shutdown, preemptive guard-page
//           suspension within budget, foreign-fault chaining, named
//           rejects for every bounded resource.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <signal.h>
#include <ucontext.h>

#include "vortex/infra/observability.hpp"
#include "vortex/support/result.hpp"

namespace vortex {

namespace vm {
class Interpreter;
}

namespace infra {

// The poll word is a plain 32-bit atomic shared with hand-written J1 asm;
// every C++ consumer relies on lock-free loads/stores (docs/
// infrastructure/04 section 3.3).
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "the safepoint poll word must be lock-free in C++ (docs/"
              "infrastructure/04 3.3)");

// ---- shared vocabulary -----------------------------------------------------

/// Why a suspension was requested (consumer-visible kind, Rule 120 dumps).
enum class SuspensionKind : uint8_t {
    Gc = 0,
    Deopt = 1,
    Profiling = 2,
    Debugger = 3,
    None = 4,
};

/// Handshake id meaning "every registered thread" (freeze_all's target).
inline constexpr uint64_t kAllHandshakeThreads = 0xFFFF'FFFFull;

/// Map a SuspensionKind to its stable observability name (never guessed).
const char* suspension_kind_name(SuspensionKind kind) noexcept;

// ---- SafepointPollingPage ----------------------------------------------------
// CEP:WHAT: Process-wide safepoint polling page pair: page A (flag page,
//           always readable) holds the global poll word, the armed kind and
//           an arm generation; page B (trap page) flips PROT_NONE when armed
//           for future trap-based consumers (J3/J4 emitted polls).
// CEP:WHY: Zero-cost safepoints without CMP/JMP sequences (docs/
//           infrastructure/04 section 2): cooperative tiers test one word;
//           the trap page reserves the protection-flip contract.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: mmap/mprotect failures reject by name with state unchanged
//           (arm leaves the word 0; disarm already cleared it and marks the
//           page degraded — later arms reject PollingPageDegraded).
// CEP:ASSUMES: One page pair per process (reserve is idempotent); arm and
//           disarm are serialized against each other by an internal mutex;
//           pollers read the word atomically and never observe a torn value.
// CEP:COST: @cold. reserve = one mmap + one mprotect; arm/disarm = one
//           mprotect + atomic stores. The consumer fast path is one relaxed
//           load of the word.
// CEP:EVIDENCE: tests/test_threading.cpp — reserve idempotence, protection
//           probe (read() from /dev/zero INTO the trap page faults EFAULT
//           when armed — a write() to /dev/null never reads the source and
//           would silently pass on a hostile page), word/kind/generation
//           states, degradation rejects.
class SafepointPollingPage {
public:
    /// Observed page state (tests + telemetry; Rule 120 spellable dumps).
    struct State {
        bool armed = false;
        SuspensionKind kind = SuspensionKind::None;
        uint64_t generation = 0;
    };

    /// Map the page pair (idempotent). Returns the flag-page base.
    static support::Result<void*> reserve();

    /// The global poll word; nullptr until the first reserve(). Never
    /// allocates — safe from hot paths.
    static std::atomic<uint32_t>* poll_word() noexcept;

    /// Raw u32 view of the poll word for hand-written asm consumers (the
    /// J1 stencil); nullptr until reserved. No C++ access happens through
    /// this pointer — C++ readers use poll_word() or std::atomic_ref over
    /// the pointee (see the header CEP:ASSUMES).
    static uint32_t* poll_word_raw() noexcept;

    /// The trap page base; nullptr until reserved. Future emitted polls
    /// load from here.
    static void* trap_page() noexcept;

    /// Arm: flip the trap page hostile, then publish kind/generation/word
    /// (release order: hostile BEFORE the word turns nonzero).
    static support::Result<void> arm(SuspensionKind kind);

    /// Disarm: clear the word, then make the trap page readable again
    /// (release order: word 0 BEFORE readable — an unblocked consumer never
    /// races the flip).
    static support::Result<void> disarm();

    static State state() noexcept;

private:
    static support::Result<void> ensure_reserved();
};

// ---- HandshakeManager --------------------------------------------------------
// CEP:WHAT: The real request/acknowledge handshake over a bounded thread
//           table: post-only request, per-thread acknowledge, bounded
//           wait_acknowledged, release, and the freeze_all/thaw_all
//           stop-the-world choreography over the polling page.
// CEP:WHY: Rule 81 — mutators must reach safepoints within bounded time and
//           the runtime needs a signal-free asymmetric protocol; LDPT's
//           patch freeze already consumes this API shape (M2 stub) and the
//           GC background batch will consume freeze_all.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Named rejects: unknown/unregistered ids, table exhaustion
//           (kMaxThreads), HandshakeTimeout (wait exceeded its budget),
//           AlreadyFrozen (concurrent freeze_all), UnsupportedPlatform.
//           request() NEVER blocks (LDPT posts request -> patch ->
//           acknowledge on one thread and must not deadlock).
// CEP:ASSUMES: Id 0 is a permanent anonymous slot (the LDPT flow uses it
//           without registration). wait_acknowledged only succeeds for
//           threads guaranteed to poll — threads that may not poll belong
//           to the guard-page path. freeze_all waits on the alive set
//           snapshot taken at request time.
// CEP:COST: @cold. request/acknowledge are atomic stores + one notify_all;
//           waiters park on one condvar with a deadline.
// CEP:EVIDENCE: tests/test_threading.cpp — cross-thread round trip, timeout
//           reject with measured latency, freeze_all bounded latency over
//           polling fibers, anonymous-0 flow (LDPT), named rejects.
class HandshakeManager {
public:
    /// The permanent anonymous slot id (LDPT's wildcard; never registered).
    static constexpr uint64_t kAnonymousId = 0;
    /// Bounded table (Rule 28).
    static constexpr size_t kMaxThreads = 4096;
    /// Default bounded wait (Rule 81); callers may pass tighter budgets.
    static constexpr std::chrono::nanoseconds kDefaultWait{10'000'000'000};

    support::Result<uint64_t> register_thread();
    support::Result<void> unregister_thread(uint64_t tid);
    support::Result<void> request(SuspensionKind kind, uint64_t tid);
    support::Result<void> acknowledge(uint64_t tid);
    support::Result<void> wait_acknowledged(
        uint64_t tid, std::chrono::nanoseconds timeout = kDefaultWait);
    support::Result<void> release(uint64_t tid);

    /// STW primitive: arm the polling page, request every alive registered
    /// thread, wait for every ack within ONE shared budget. Registration is
    /// rejected for the whole freeze window (a late registrant cannot run
    /// un-waited inside a "stopped" world). On timeout the freeze is rolled
    /// back (thaw) and the timeout rejects by name. A thread that EXITS
    /// during the window satisfies the freeze (it can no longer mutate).
    /// Returns the freeze generation (observability; v1 does not gate
    /// thaw_all on it — any subsystem may end the freeze, documented in
    /// docs/infrastructure/04 section 4).
    support::Result<uint64_t> freeze_all(
        SuspensionKind kind, std::chrono::nanoseconds timeout = kDefaultWait);
    /// Disarm + release every thread; clears the frozen flag.
    support::Result<void> thaw_all();

    bool pending(uint64_t tid) const noexcept;
    bool frozen() const noexcept;

    /// TTSP telemetry sink (docs/infrastructure/06 section 5): freezes and
    /// single-thread handshakes record cause/TTSP/pause-body events. Raw
    /// pointer, no ownership, null default (zero cost when unset); the log
    /// is called under this manager's own serialization and never fails the
    /// handshake. Guard-page (preemptive) events join with the emitted-poll
    /// batch (R31).
    void set_safepoint_log(SafepointLog* log) noexcept {
        safepoint_log_ = log;
    }

    struct Stats {
        uint64_t requests = 0;
        uint64_t freezes = 0;
        uint64_t timeouts = 0;
    };
    Stats stats() const noexcept;

private:
    struct Slot {
        std::atomic<uint32_t> pending{0};  // 0 none, else kind+1
        std::atomic<uint32_t> acked{0};
        std::atomic<uint32_t> alive{0};
        /// TTSP telemetry (docs/infrastructure/06 section 5): request()
        /// stamps the monotonic time and the kind; the waiter computes the
        /// delta on ack. Stale values between requests are benign (restamped
        /// per request).
        std::atomic<uint64_t> requested_ns{0};
        std::atomic<uint32_t> requested_kind{0};
    };
    // Slots indexed by id-1; slot 0 internal = the anonymous slot.
    support::Result<Slot*> slot_for(uint64_t tid) noexcept;

    // Register/unregister/freeze bookkeeping serialization (short, bounded
    // critical sections; never taken by the trap handler).
    mutable std::mutex mu_;
    std::condition_variable cv_;
    Slot slots_[kMaxThreads];
    uint64_t freeze_generation_ = 0;
    std::atomic<bool> frozen_{false};
    std::atomic<uint64_t> stat_requests_{0};
    std::atomic<uint64_t> stat_freezes_{0};
    std::atomic<uint64_t> stat_timeouts_{0};

    // TTSP telemetry (mu_-guarded pending-freeze bookkeeping; docs/
    // infrastructure/06 section 5). The Freeze event is recorded when the
    // body completes (thaw_all) or the wait rolls back (timeout).
    SafepointLog* safepoint_log_ = nullptr;
    uint64_t freeze_ttsp_ns_ = 0;
    uint64_t freeze_start_ns_ = 0;
    uint32_t freeze_threads_ = 0;
    uint8_t freeze_cause_ = 0;
    bool freeze_event_pending_ = false;

    // Emits the pending Freeze event under mu_ (or the given explicit
    // fields for the rollback path); no-op without a sink.
    void emit_freeze_locked(uint64_t ttsp_ns, uint64_t body_ns,
                            uint32_t threads, uint8_t cause,
                            bool timed_out, uint64_t generation);
};

// ---- GuardPageSuspender ------------------------------------------------------
// CEP:WHAT: Preemptive suspension through stack guard pages: registry of
//           PROT_NONE page regions each bound to a Record (park point +
//           captured context), one SIGSEGV handler (SA_ONSTACK over a
//           runtime-owned altstack) that captures the interrupted context
//           and siglongjmps to the record's park point; foreign faults
//           chain to the previous handler. resume() disarms and
//           swapcontext's back into the captured context.
// CEP:WHY: Rule 81 for code that never polls: the next guest push below the
//           armed page traps, suspending the thread within one frame push —
//           no OS signals delivered to guest code, no polling loop needed.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Named rejects: zero/oversized regions, overlapping armed
//           regions, registry exhaustion (kMaxRegions), enable/disable
//           lifecycle misuse, resume of a non-suspended record,
//           UnsupportedPlatform. mprotect failures leave the registry
//           unchanged (the registry entry publishes BEFORE the protect so a
//           racing trap always finds its record).
// CEP:ASSUMES: Linux x86-64; enable_trap_suspension installs over the
//           previous SIGSEGV disposition and CHAINS foreign faults by
//           calling a SA_SIGINFO predecessor DIRECTLY (the handler stays
//           installed — restoring and returning would uninstall it the
//           first time a sanitizer report returns); only a default
//           disposition restores. The handler runs async-signal-safe
//           operations only: atomics, memcpy, siglongjmp — no stdio, no
//           malloc, no locks, no TLS reads beyond the per-thread altstack
//           bound. The chain-target snapshot (old_sa) is written under
//           sched_mu; the handler reads it lock-free — an accepted benign
//           race: a stale read either sees the previous foreign handler or
//           the self-refusal check rejects it. Per-thread 64 KiB altstacks
//           (SIGSTKSZ*2 overflows under sanitizer interceptor frames). The
//           captured FP state is copied out of the signal frame into the
//           record (4096-byte bound). Exactly one live context per record
//           at any time. A record's lifetime_base (the fiber's low
//           overflow guard) is NEVER suspended on: the handler fails the
//           fiber by name instead — disarming a lifetime guard would turn
//           a bounded stack overflow into silent memory corruption.
// CEP:COST: @cold for arm/disarm (one mprotect + bounded registry scan).
//           The trap path is one bounded registry scan in the handler.
// CEP:EVIDENCE: tests/test_threading.cpp — preemptive suspension of a
//           no-poll fiber within budget, resume with loop state intact,
//           foreign-fault chaining (fork-based), region rejects, the
//           lifetime-guard fault failing its fiber by name.
class GuardPageSuspender {
public:
    /// Bounded region registry (Rule 28).
    static constexpr size_t kMaxRegions = 1024;
    /// Bound on the copied xsave FP area from the signal frame (x86-64).
    static constexpr size_t kFpStateBytes = 4096;

    /// Suspension record owned by the suspendee's scheduler (a Fiber or a
    /// bare-thread park owner). Park is taken with sigsetjmp(park, 1) by
    /// whoever is about to run the record.
    struct Record {
        sigjmp_buf park{};
        // 0 idle, 1 running, 2 suspended, 3 faulted (constants below).
        std::atomic<uint32_t> state{0};
        uint64_t token = 0;  // opaque owner token (e.g. fiber id)
        bool captured_valid = false;
        // When non-null, the region ARMED AT EXACTLY THIS BASE is the
        // owner's LIFETIME low stack guard: a fault there is a stack
        // overflow — the handler marks kStateFaulted and fails the owner,
        // never suspends. Suspension patches armed over the SAME record
        // (different bases) keep suspending — the park stays shared.
        unsigned char* lifetime_base = nullptr;
        unsigned char* fault_region = nullptr;  // armed base that trapped
        alignas(64) unsigned char uc_buf[sizeof(ucontext_t)]{};
        alignas(64) unsigned char fp_buf[kFpStateBytes]{};
    };

    static constexpr uint32_t kStateIdle = 0;
    static constexpr uint32_t kStateRunning = 1;
    static constexpr uint32_t kStateSuspended = 2;
    static constexpr uint32_t kStateFaulted = 3;

    static bool trap_suspension_enabled() noexcept;

    /// Install the trap handler over the previous disposition (chained).
    /// Idempotent; a second enable is a success no-op.
    static support::Result<void> enable_trap_suspension();
    /// Restore the previous disposition. Idempotent.
    static support::Result<void> disable_trap_suspension();

    /// Arm [base, base+size) (page-aligned outward) as a guard region.
    static support::Result<void> arm(void* base, size_t size, Record* record);
    /// Arm the single page strictly below the page containing sp_hint —
    /// the suspension backstop for a descending guest stack.
    static support::Result<void> arm_sp_patch(Record* record, void* sp_hint);
    /// Disarm the region armed FOR this record (its base differs from any
    /// other armed base). Rejects when nothing is armed for it.
    static support::Result<void> disarm_record(Record* record);

    /// Disarm the region starting at `base` (page-aligned as armed).
    static support::Result<void> disarm(void* base);

    /// Resume a suspended record: disarm its regions, then swapcontext
    /// into the captured context (comeback receives the caller's state;
    /// the record re-executes the faulting access with the page readable).
    static support::Result<void> resume(Record* record, ucontext_t* comeback);

    /// Test/observability: is any armed region containing this address?
    static bool armed_for(const void* addr) noexcept;

    /// Re-assert this runtime's SIGSEGV disposition on the CALLING thread's
    /// process-wide table when a host (e.g. a sanitizer runtime) displaced
    /// it after enable(). The displaced handler becomes the chain target.
    /// Cheap no-op when the disposition is already ours. Workers call this
    /// at startup; embedders that create threads after enable() should too.
    static void ensure_installed() noexcept;

    /// The one SIGSEGV disposition installed by enable_trap_suspension
    /// (plain function pointer via static member; never throws).
    static void trap_signal_handler(int signo, siginfo_t* info,
                                    void* uctx) noexcept;
};

// ---- WorkStealingPool (M:N scheduler) ----------------------------------------
// CEP:WHAT: M:N scheduler: ucontext fibers on mmap'd stacks (guard page at
//           the low end) multiplexed onto N OS worker threads; per-worker
//           mutex deques with randomized stealing, cooperative yield at
//           safepoint polls, join with entry-result propagation, bounded
//           drain shutdown, and safepoint integration (handshake ack +
//           cooperative freeze wait + quantum rotation).
// CEP:WHY: The guest concurrency model (docs/infrastructure/04 section 1):
//           many guest fibers over a fixed OS pool, with suspension that
//           cooperates with the handshake/polling substrate.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Named rejects: invalid thread count, spawn after shutdown,
//           stack below the minimum, fiber table exhaustion (kMaxFibers),
//           yield/join off a fiber context where meaningless, join of an
//           unknown id, shutdown with suspended fibers
//           (ShutdownWithSuspendedFibers), degraded pool after worker
//           spawn failure.
// CEP:ASSUMES: v1 deques are mutex-guarded (lock-free Chase-Lev upgrade is
//           tracked in the roadmap). Exceptions never cross a fiber
//           boundary (Rule 114) — entries return Result and the unexpected
//           surfaces through join. Workers are plain std::threads; fibers
//           never migrate mid-execution (migration happens at yield/resume
//           points only). Fiber stacks own their low guard page for the
//           fiber's lifetime.
// CEP:COST: @cold except the poll hook (one relaxed load + rare slow path)
//           and yield (one swapcontext + small queue push). Worker idle
//           parks on one condvar.
// CEP:EVIDENCE: tests/test_threading.cpp — 32 fibers over 2 workers with
//           steals, deterministic yield rotation, join result propagation,
//           shutdown drain, freeze over polling fibers, preemptive
//           suspension + resume through the pool.
class WorkStealingPool {
public:
    using Entry = support::Result<void> (*)(void* user);

    /// Bounded fiber table (Rule 28); the roadmap's "millions" was
    /// aspirational prose — 65536 live fibers is the shipped bound.
    static constexpr size_t kMaxFibers = 65536;
    static constexpr size_t kMinStackBytes = 64 * 1024;
    static constexpr size_t kDefaultStackBytes = 256 * 1024;
    /// Cooperative rotation quantum: every kYieldQuantum executed polls the
    /// fiber yields to its worker (M:N interleaving).
    static constexpr uint32_t kYieldQuantum = 1024;

    /// os_threads == 0 selects DRIVER-ONLY mode: a valid pool with no
    /// worker threads — fibers are run by the embedder through
    /// resume_fiber (deterministic driving; suspension faults then fire on
    /// the caller's thread with the caller's own trap disposition).
    explicit WorkStealingPool(uint32_t os_threads);
    ~WorkStealingPool();
    WorkStealingPool(const WorkStealingPool&) = delete;
    WorkStealingPool& operator=(const WorkStealingPool&) = delete;

    /// One spawn request; id/stack_base/state are filled on success (the
    /// by-reference contract — the struct is the out-param channel).
    struct GreenThread {
        Entry entry = nullptr;
        void* user = nullptr;
        size_t stack_size = 0;       // 0 = kDefaultStackBytes
        uint64_t id = 0;             // out
        void* stack_base = nullptr;  // out: low end of the usable stack
                                     // (adjacent to the lifetime guard;
                                     // frames descend toward it from
                                     // stack_base + stack_size)
        uint32_t state = 0;          // out mirror
    };
    support::Result<uint64_t> spawn(GreenThread& thread);
    support::Result<uint64_t> spawn(Entry entry, void* user,
                                    size_t stack_bytes = kDefaultStackBytes);

    /// Cooperative yield from the RUNNING fiber back to its worker (or the
    /// resume point); requeues Ready. Rejects off a fiber context.
    support::Result<void> yield_current();

    /// Request preemptive suspension of a fiber that may never poll: arms
    /// one stack page strictly below the fiber's basis — the RSP recorded at
    /// its LAST park (the entry trampoline's frame for a never-started
    /// fiber, re-based at every yield). The request lands when the fiber's
    /// descent next crosses that page — a fiber that descended below its
    /// last park basis and loops there without crossing it needs the
    /// cooperative poll path instead. Rejects for unknown/done ids.
    support::Result<void> suspend_fiber(uint64_t fid);

    /// Observability (Rule 120): the fiber's scheduler state — 0 Ready,
    /// 1 Running, 2 Suspended, 3 Done. Rejects unknown ids.
    support::Result<uint32_t> fiber_state(uint64_t fid) const;

    /// Outcome of the current swap-in round for a fiber.
    enum class RunOutcome : uint8_t { Yielded, Done, Suspended };

    /// Run a fiber until its next yield/done/trap. Callable from any
    /// thread; workers use it in their loop, tests drive it directly.
    support::Result<RunOutcome> resume_fiber(uint64_t fid);

    /// Block until the fiber is Done (fibers yield; OS threads park on the
    /// pool condvar). Propagates the entry's unexpected.
    support::Result<void> join(uint64_t fid);

    /// Wire the safepoint integration: fibers acknowledge the handshake at
    /// polls, wait out freezes cooperatively, and rotate every quantum.
    support::Result<void> bind_suspension(HandshakeManager* handshake,
                                          SafepointPollingPage* page);

    /// Install the pool's poll hook into an interpreter (bind_suspension
    /// first). Adopted interpreters are NOT owned by the pool.
    support::Result<void> adopt_interpreter(vm::Interpreter* interpreter);

    support::Result<void> shutdown();
    bool shutdown_complete() const noexcept;

    struct Stats {
        uint64_t spawned = 0;
        uint64_t completed = 0;
        uint64_t stolen = 0;
        uint64_t yields = 0;
        uint64_t freeze_polls = 0;
        uint64_t run_rejected = 0;  // workers consumed a rejected swap-in
    };
    Stats stats() const noexcept;

    uint32_t os_thread_count() const noexcept;
    uint64_t fiber_count() const noexcept;

    /// The pool's poll hook (installed into adopted interpreters).
    static void poll_hook(void* user);

    // Fiber/worker internals: plain structs, no virtuals, no node containers.
    struct Fiber;
    struct Worker;

private:
    struct Impl;
    Impl* impl_;  // pimpl: the pool owns OS threads; header stays lean

    // Worker/fiber plumbing (defined in the .cpp; static members so the
    // launch trampolines keep class access to the private internals).
    // worker_main is deliberately not noexcept (worker mutexes may report
    // system_error); the trampoline itself is.
    static void fiber_thunk(int lo, int hi) noexcept;
    static void worker_main(Worker& w);
    static void* worker_tramp(void* arg) noexcept;
};

}  // namespace infra
}  // namespace vortex
