// CEP:FILE: src/infra/threading.cpp
// CEP:WHAT: Infra 4 implementation (M6-b, docs/infrastructure/
//           04-threading-suspension.md): the safepoint polling page pair
//           (flag page + protection-flipped trap page), the real
//           HandshakeManager (bounded thread table, post-only request,
//           bounded waits, freeze_all/thaw_all stop-the-world), the
//           GuardPageSuspender (guard-region registry, one chaining
//           SIGSEGV handler over a runtime altstack, context capture with
//           the FP state copied out of the signal frame, setcontext-free
//           resume via swapcontext), and the M:N work-stealing fiber
//           scheduler (ucontext fibers on mmap'd guard-page stacks,
//           pthread workers, per-worker deques + randomized stealing,
//           cooperative safepoint integration).
// CEP:WHY: Rule 81 — safepoints reachable within bounded time with a
//           signal-free protocol; the guest concurrency model; the
//           suspension substrate the GC background batch builds on.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Every failure is a named Result reject ("threading: <what>").
//           mmap/mprotect failures name the errno and leave state unchanged
//           (or degrade the polling page with the word already cleared).
//           Handshake timeouts roll the freeze back (thaw) before failing.
//           Bounded resources (thread table 4096, guard regions 1024,
//           fibers 65536) reject by name at capacity (Rule 28). No throw,
//           no try/catch, no new/delete tokens (Rule 65); workers are
//           pthreads created with pthread_create (error-code API — no
//           exception path at pool construction).
// CEP:ASSUMES: Linux x86-64 (glibc ucontext: the signal-captured FP state
//           lives in the signal frame and is copied into the record's
//           4096-byte buffer bounded by the altstack end — tail AVX-512
//           state beyond that bound is not preserved; the guest target is
//           scalar/integer code and this is a documented limitation).
//           Entries must not raise exceptions (Rule 114 — exceptions
//           never cross a fiber boundary). The handler runs async-signal-safe operations
//           only: atomics, memcpy, siglongjmp. Foreign SIGSEGV faults chain
//           to the previous disposition (sanitizer reports keep working).
// CEP:COST: @cold except: the poll fast path (one relaxed load), yield
//           (one swapcontext + small mutex push), the trap handler (one
//           bounded registry scan). Worker idle parks on a condvar with a
//           1 ms poll — v1 simplicity over latency hair-splitting.
// CEP:EVIDENCE: tests/test_threading.cpp (full DoD list, docs/
//           infrastructure/04 section 10).
#include "vortex/infra/threading.hpp"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "vortex/vm/interpreter.hpp"

namespace vortex::infra {

namespace {

// ---- small platform helpers -------------------------------------------------

// CEP:WHAT: Platform predicate for the M6-b suspension substrate.
// CEP:WHY: Linux x86-64 only (docs/infrastructure/04 section 8).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Every entry point returns the named UnsupportedPlatform reject off-platform.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. Compile-time-constant on the target.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
bool platform_supported() noexcept {
#if defined(__linux__) && defined(__x86_64__)
    return true;
#else
    return false;
#endif
}

// CEP:WHAT: Builds the named UnsupportedPlatform diagnostic (Rule 29 degrade).
// CEP:WHY: Off-platform callers must fail loudly, never partially run.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a pure diagnostic constructor.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One Diagnostic allocation.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
std::unexpected<support::Diagnostic> platform_fail() {
    return support::fail(support::ErrorCode::Unsupported,
                         "threading: Linux x86-64 required (Rule 29 degrade)");
}

// CEP:WHAT: Cached OS page size (sysconf) for every page-aligned operation.
// CEP:WHY: mprotect/mmap rounding must match the kernel's page granularity.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — sysconf failure falls back to 4096.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One sysconf, then a cached load.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
size_t os_page_size() noexcept {
    static const size_t ps = [] {
        long v = sysconf(_SC_PAGESIZE);
        return v > 0 ? static_cast<size_t>(v) : size_t{4096};
    }();
    return ps;
}

// CEP:WHAT: Round an address down to a power-of-two boundary.
// CEP:WHY: Guard regions and the patch basis are page-aligned outward.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — pure arithmetic.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One AND.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
uintptr_t align_down(uintptr_t v, size_t a) noexcept { return v & ~uintptr_t{a - 1}; }
uintptr_t align_up(uintptr_t v, size_t a) noexcept {
    return align_down(v + uintptr_t{a - 1}, a);
}

// ---- polling page state -----------------------------------------------------

struct PageState {
    std::mutex mu;
    unsigned char* base = nullptr;  // flag page
    size_t page = 0;
    std::atomic<uint32_t>* word = nullptr;
    std::atomic<uint8_t>* kind = nullptr;
    std::atomic<uint64_t>* generation = nullptr;
    bool degraded = false;
};

// CEP:WHAT: Process-wide polling-page state singleton.
// CEP:WHY: The page pair is a process resource; one instance serves all tiers.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a static accessor.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One static access.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
PageState& page_state() noexcept {
    static PageState s;
    return s;
}

}  // namespace

// CEP:WHAT: Maps a SuspensionKind to its stable observability name (Rule 120).
// CEP:WHY: Dumps must spell kinds, never guess ordinals.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a pure enum mapping.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One switch, returns a static literal.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
const char* suspension_kind_name(SuspensionKind kind) noexcept {
    switch (kind) {
        case SuspensionKind::Gc: return "gc";
        case SuspensionKind::Deopt: return "deopt";
        case SuspensionKind::Profiling: return "profiling";
        case SuspensionKind::Debugger: return "debugger";
        case SuspensionKind::None: return "none";
    }
    return "none";
}

// ---- SafepointPollingPage ---------------------------------------------------

// CEP:WHAT: Maps the two-page polling pair and publishes the flag-page accessors.
// CEP:WHY: reserve is idempotent; the word starts zero (disarmed).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: mmap failure -> named RuntimeError, state unchanged.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One mmap + three placement-new atomics.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void*> SafepointPollingPage::reserve() {
    if (!platform_supported()) return platform_fail();
    PageState& p = page_state();
    std::lock_guard<std::mutex> lk(p.mu);
    if (p.base != nullptr) return p.base;
    p.page = os_page_size();
    void* mem = mmap(nullptr, 2 * p.page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return support::fail(support::ErrorCode::RuntimeError,
                             "threading: polling page mmap failed (errno " +
                                 std::to_string(errno) + ")");
    }
    p.base = static_cast<unsigned char*>(mem);
    p.word = new (p.base) std::atomic<uint32_t>{0};
    p.kind = new (p.base + 4) std::atomic<uint8_t>{
        static_cast<uint8_t>(SuspensionKind::None)};
    p.generation = new (p.base + 8) std::atomic<uint64_t>{0};
    return p.base;
}

// CEP:WHAT: Lazily reserves the page pair for arm().
// CEP:WHY: arm() is callable before an explicit reserve().
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Propagates reserve's named failures.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One idempotent reserve.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> SafepointPollingPage::ensure_reserved() {
    if (page_state().base == nullptr) {
        auto r = reserve();
        if (!r) return std::unexpected(std::move(r).error());
    }
    return support::ok();
}

// CEP:WHAT: Returns the global poll word; nullptr until the first reserve().
// CEP:WHY: The consumer fast path is one relaxed load of this word.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — never allocates.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @hot-adjacent. One static access.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
std::atomic<uint32_t>* SafepointPollingPage::poll_word() noexcept {
    return page_state().word;
}

// CEP:WHAT: Raw u32 view of the poll word for the J1 stencil's asm load.
// CEP:WHY: Hand-written asm dereferences it; no C++ access happens through this view.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a pointer view.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One reinterpret.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
uint32_t* SafepointPollingPage::poll_word_raw() noexcept {
    // Raw view for the J1 stencil (hand-written asm dereferences it; no C++
    // access happens through this pointer — see the header CEP:ASSUMES).
    return reinterpret_cast<uint32_t*>(page_state().word);
}

// CEP:WHAT: Returns the trap page base (nullptr until reserved).
// CEP:WHY: Future emitted polls load from here; tests probe its protection.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — an accessor.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One static access.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
void* SafepointPollingPage::trap_page() noexcept {
    PageState& p = page_state();
    return p.base != nullptr ? p.base + p.page : nullptr;
}

// CEP:WHAT: Flips the trap page hostile, then publishes kind/generation/word.
// CEP:WHY: Ordering: hostile BEFORE the word turns nonzero (release).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: mprotect failure -> named reject, word left 0; degraded page -> PollingPageDegraded.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One mprotect + atomic stores.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> SafepointPollingPage::arm(SuspensionKind kind) {
    if (!platform_supported()) return platform_fail();
    if (auto r = ensure_reserved(); !r) return r;
    PageState& p = page_state();
    std::lock_guard<std::mutex> lk(p.mu);
    if (p.degraded) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: polling page degraded by a failed "
                             "disarm protect (PollingPageDegraded)");
    }
    // Hostile trap page FIRST, then the word (release ordering: a consumer
    // that observes the armed word can rely on the trap page being armed).
    if (mprotect(p.base + p.page, p.page, PROT_NONE) != 0) {
        return support::fail(support::ErrorCode::RuntimeError,
                             "threading: polling page arm mprotect failed "
                             "(errno " +
                                 std::to_string(errno) +
                                 "); word left disarmed");
    }
    p.generation->fetch_add(1, std::memory_order_relaxed);
    p.kind->store(static_cast<uint8_t>(kind), std::memory_order_relaxed);
    p.word->store(0x8000'0000u | (static_cast<uint32_t>(kind) << 1) | 1u,
                  std::memory_order_release);
    return support::ok();
}

// CEP:WHAT: Clears the word, then makes the trap page readable again.
// CEP:WHY: Ordering: word 0 BEFORE readable — an unblocked poll never races the flip.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: mprotect failure -> named reject, page degraded, word already 0.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One atomic store + mprotect.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> SafepointPollingPage::disarm() {
    if (!platform_supported()) return platform_fail();
    PageState& p = page_state();
    std::lock_guard<std::mutex> lk(p.mu);
    if (p.base == nullptr) return support::ok();  // never reserved: no-op
    // Word clears FIRST (an unblocked consumer never races the flip).
    p.word->store(0, std::memory_order_release);
    p.kind->store(static_cast<uint8_t>(SuspensionKind::None),
                  std::memory_order_relaxed);
    if (mprotect(p.base + p.page, p.page, PROT_READ | PROT_WRITE) != 0) {
        p.degraded = true;
        return support::fail(support::ErrorCode::RuntimeError,
                             "threading: polling page disarm mprotect failed "
                             "(errno " +
                                 std::to_string(errno) +
                                 "); word cleared, page degraded");
    }
    return support::ok();
}

// CEP:WHAT: Observes the page state (armed/kind/generation) for tests and dumps.
// CEP:WHY: The kind byte is read separately (the zero word would alias Gc).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — an atomic snapshot.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. Three atomic loads.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
SafepointPollingPage::State SafepointPollingPage::state() noexcept {
    PageState& p = page_state();
    State s;
    if (p.base == nullptr) return s;
    uint32_t w = p.word->load(std::memory_order_acquire);
    s.armed = w != 0;
    // The kind lives in its own byte (the zero word would alias Gc).
    s.kind = s.armed ? static_cast<SuspensionKind>(
                            p.kind->load(std::memory_order_relaxed))
                     : SuspensionKind::None;
    s.generation = p.generation->load(std::memory_order_relaxed);
    return s;
}

// ---- HandshakeManager -------------------------------------------------------

// CEP:WHAT: Slot resolution shared by the protocol methods.
// CEP:WHY: Id 0 is the permanent anonymous slot; ids are 1-based over the table.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: kAllHandshakeThreads / out-of-range ids -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One bounds check.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<HandshakeManager::Slot*> HandshakeManager::slot_for(
    uint64_t tid) noexcept {
    if (tid == kAllHandshakeThreads) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: kAllHandshakeThreads is not a slot id");
    }
    if (tid == kAnonymousId) return &slots_[0];
    if (tid - 1 >= kMaxThreads) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: handshake id out of range: " +
                                 std::to_string(tid));
    }
    return &slots_[tid - 1];
}

// CEP:WHAT: Claims a free thread-table slot and returns its id.
// CEP:WHY: Slots recycle after unregister; capacity is bounded (Rule 28).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Table exhaustion -> OutOfMemory named reject.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One CAS scan.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<uint64_t> HandshakeManager::register_thread() {
    if (!platform_supported()) return platform_fail();
    std::lock_guard<std::mutex> lk(mu_);
    // Rejected for the whole freeze window (M6-b review M5): a thread that
    // registers mid-freeze would run as a mutator inside a "stopped" world
    // — the freeze's wait set is the alive snapshot and a late registrant
    // is not in it.
    if (frozen_.load(std::memory_order_acquire)) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: register during a freeze window");
    }
    for (size_t i = 1; i < kMaxThreads; ++i) {
        uint32_t expected = 0;
        if (slots_[i].alive.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel)) {
            slots_[i].pending.store(0, std::memory_order_relaxed);
            slots_[i].acked.store(0, std::memory_order_relaxed);
            return static_cast<uint64_t>(i + 1);
        }
    }
    return support::fail(support::ErrorCode::OutOfMemory,
                         "threading: handshake thread table full (4096)");
}

// CEP:WHAT: Marks a thread slot dead and wakes its waiters.
// CEP:WHY: Released ids are immediately reusable; waiters fail by name.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Unknown id / the anonymous slot -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One atomic exchange + notify.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> HandshakeManager::unregister_thread(uint64_t tid) {
    if (!platform_supported()) return platform_fail();
    if (tid == kAnonymousId) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: the anonymous slot is permanent");
    }
    auto s = slot_for(tid);
    if (!s) return std::unexpected(std::move(s).error());
    std::lock_guard<std::mutex> lk(mu_);
    if (s.value()->alive.exchange(0, std::memory_order_acq_rel) == 0) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: unregister of an unregistered id: " +
                                 std::to_string(tid));
    }
    s.value()->pending.store(0, std::memory_order_relaxed);
    s.value()->acked.store(0, std::memory_order_relaxed);
    cv_.notify_all();
    return support::ok();
}

// CEP:WHAT: Posts a suspension request (pending = kind+1); NEVER blocks.
// CEP:WHY: LDPT posts request -> patch -> acknowledge on one thread; kAll targets every alive slot.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Unregistered ids -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One atomic store + notify.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> HandshakeManager::request(SuspensionKind kind,
                                                uint64_t tid) {
    if (!platform_supported()) return platform_fail();
    if (tid == kAllHandshakeThreads) {
        stat_requests_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t now = infra::mono_clock_ns();
        for (size_t i = 1; i < kMaxThreads; ++i) {
            if (slots_[i].alive.load(std::memory_order_acquire) != 0) {
                slots_[i].requested_ns.store(now, std::memory_order_relaxed);
                slots_[i].requested_kind.store(
                    static_cast<uint32_t>(kind) + 1, std::memory_order_relaxed);
                slots_[i].pending.store(static_cast<uint32_t>(kind) + 1,
                                        std::memory_order_release);
            }
        }
        cv_.notify_all();
        return support::ok();
    }
    auto s = slot_for(tid);
    if (!s) return std::unexpected(std::move(s).error());
    Slot* slot = s.value();
    if (tid != kAnonymousId &&
        slot->alive.load(std::memory_order_acquire) == 0) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: request to an unregistered id: " +
                                 std::to_string(tid));
    }
    // Counted on acceptance, not on entry (M6-b review m1: rejected
    // requests must not inflate the counter — Rule 120 honesty).
    stat_requests_.fetch_add(1, std::memory_order_relaxed);
    // TTSP stamp (docs/infrastructure/06 section 5): the waiter computes
    // the request->ack delta from these fields.
    slot->requested_ns.store(infra::mono_clock_ns(), std::memory_order_relaxed);
    slot->requested_kind.store(static_cast<uint32_t>(kind) + 1,
                               std::memory_order_relaxed);
    slot->pending.store(static_cast<uint32_t>(kind) + 1,
                        std::memory_order_release);
    cv_.notify_all();
    return support::ok();
}

// CEP:WHAT: Records that a thread reached its safepoint; wakes waiters.
// CEP:WHY: Waiters park on one condvar with a deadline (Rule 81).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Unregistered ids -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. Two atomic stores + notify.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> HandshakeManager::acknowledge(uint64_t tid) {
    if (!platform_supported()) return platform_fail();
    if (tid == kAllHandshakeThreads) {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < kMaxThreads; ++i) {
            slots_[i].pending.store(0, std::memory_order_relaxed);
            slots_[i].acked.store(1, std::memory_order_release);
        }
        cv_.notify_all();
        return support::ok();
    }
    auto s = slot_for(tid);
    if (!s) return std::unexpected(std::move(s).error());
    Slot* slot = s.value();
    if (tid != kAnonymousId &&
        slot->alive.load(std::memory_order_acquire) == 0) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: acknowledge of an unregistered id: " +
                                 std::to_string(tid));
    }
    // State change + notify under mu_: the waiter checks the predicate
    // under mu_ and sleeps releasing it — a notify outside the lock can
    // land in the check->sleep window and be lost (probe-proven spurious
    // HandshakeTimeout).
    std::lock_guard<std::mutex> lk(mu_);
    slot->pending.store(0, std::memory_order_relaxed);
    slot->acked.store(1, std::memory_order_release);
    cv_.notify_all();
    return support::ok();
}

// CEP:WHAT: Bounded block until the slot's acked flag is set.
// CEP:WHY: A stuck thread fails the freeze loudly within the budget (Rule 81).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Deadline miss -> HandshakeTimeout; unregistered id -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One condvar wait_until.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> HandshakeManager::wait_acknowledged(
    uint64_t tid, std::chrono::nanoseconds timeout) {
    if (!platform_supported()) return platform_fail();
    auto s = slot_for(tid);
    if (!s) return std::unexpected(std::move(s).error());
    Slot* slot = s.value();
    if (tid != kAnonymousId &&
        slot->alive.load(std::memory_order_acquire) == 0) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: wait on an unregistered id: " +
                                 std::to_string(tid));
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lk(mu_);
    const bool done = cv_.wait_until(lk, deadline, [&] {
        return slot->acked.load(std::memory_order_acquire) != 0;
    });
    if (!done) {
        stat_timeouts_.fetch_add(1, std::memory_order_relaxed);
        if (getenv("VORTEX_THREADING_DEBUG") != nullptr) {
            char b[160];
            int n = snprintf(b, sizeof(b),
                             "[wait-timeout] tid=%llu pending=%u acked=%u "
                             "alive=%u\n",
                             (unsigned long long)tid,
                             slot->pending.load(), slot->acked.load(),
                             slot->alive.load());
            write(2, b, n);
        }
        return support::fail(
            support::ErrorCode::RuntimeError,
            "threading: handshake timeout waiting for thread " +
                std::to_string(tid) + " (Rule 81: bounded, fails loudly)");
    }
    // TTSP telemetry (docs/infrastructure/06 section 5): one Handshake
    // event per successful bounded wait, recorded UNDER the wait's lock —
    // the ring push is serialized by mu_ (multi-waiter safety) and no
    // relock is needed (a second lock on the same non-recursive mutex
    // would self-deadlock).
    if (safepoint_log_ != nullptr) {
        SafepointEvent event;
        event.kind = SafepointEvent::Kind::Handshake;
        const uint32_t kind_stamp =
            slot->requested_kind.load(std::memory_order_relaxed);
        event.cause = kind_stamp != 0 ? static_cast<uint8_t>(kind_stamp - 1)
                                      : 0;
        event.ttsp_ns =
            infra::mono_clock_ns() -
            slot->requested_ns.load(std::memory_order_relaxed);
        event.body_ns = 0;
        event.threads = 1;
        event.timed_out = false;
        event.generation = 0;
        safepoint_log_->record(event);
    }
    return support::ok();
}

// CEP:WHAT: Clears pending+acked so the thread resumes its poll loop.
// CEP:WHY: Paired with freeze_all in the STW choreography.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Unregistered ids -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. Two atomic stores + notify.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> HandshakeManager::release(uint64_t tid) {
    if (!platform_supported()) return platform_fail();
    if (tid == kAllHandshakeThreads) {
        for (size_t i = 0; i < kMaxThreads; ++i) {
            slots_[i].pending.store(0, std::memory_order_relaxed);
            slots_[i].acked.store(0, std::memory_order_release);
        }
        cv_.notify_all();
        return support::ok();
    }
    auto s = slot_for(tid);
    if (!s) return std::unexpected(std::move(s).error());
    Slot* slot = s.value();
    if (tid != kAnonymousId &&
        slot->alive.load(std::memory_order_acquire) == 0) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: release of an unregistered id: " +
                                 std::to_string(tid));
    }
    std::lock_guard<std::mutex> lk(mu_);
    slot->pending.store(0, std::memory_order_relaxed);
    slot->acked.store(0, std::memory_order_relaxed);
    cv_.notify_all();
    return support::ok();
}

// CEP:WHAT: Stop-the-world: arm the page, request every alive thread, wait all acks.
// CEP:WHY: Rollback on timeout (thaw) so a failed freeze never wedges the system.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: AlreadyFrozen / HandshakeTimeout / arm failure -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One arm + N bounded waits.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<uint64_t> HandshakeManager::freeze_all(
    SuspensionKind kind, std::chrono::nanoseconds timeout) {
    if (!platform_supported()) return platform_fail();
    std::vector<uint64_t> alive_ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // CAS gate (M6-b review M4): the check and the publication are one
        // transition — two concurrent freeze_all calls can never both pass
        // (the loser rejects AlreadyFrozen).
        bool expected = false;
        if (!frozen_.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return support::fail(support::ErrorCode::InvalidState,
                                 "threading: already frozen (AlreadyFrozen)");
        }
        for (size_t i = 1; i < kMaxThreads; ++i) {
            if (slots_[i].alive.load(std::memory_order_acquire) != 0) {
                alive_ids.push_back(static_cast<uint64_t>(i + 1));
            }
        }
    }
    // One SHARED deadline for every wait (M6-b review m2): the worst case
    // is the caller's budget, never N x budget.
    // TTSP clock start BEFORE the deadline: ttsp = freeze entry -> last
    // acknowledge, so ttsp >= the caller's budget holds by construction
    // (docs/infrastructure/06 section 5).
    const uint64_t ttsp_t0 = infra::mono_clock_ns();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // Arm FIRST: a failure here means nothing was requested.
    if (auto r = SafepointPollingPage::arm(kind); !r) {
        std::lock_guard<std::mutex> lk(mu_);
        frozen_.store(false, std::memory_order_release);
        return std::unexpected(std::move(r).error());
    }
    if (auto r = request(kind, kAllHandshakeThreads); !r) {
        (void)SafepointPollingPage::disarm();
        std::lock_guard<std::mutex> lk(mu_);
        frozen_.store(false, std::memory_order_release);
        return std::unexpected(std::move(r).error());
    }
    for (uint64_t id : alive_ids) {
        auto s = slot_for(id);
        if (!s) continue;  // snapshot id vanished — nothing to wait for
        Slot* slot = s.value();
        std::unique_lock<std::mutex> lk(mu_);
        // Predicate under mu_ (the probe-proven lost-wakeup discipline).
        // A thread that EXITS during the window SATISFIES the freeze (M6-b
        // review m2): it can no longer mutate — failing the whole freeze
        // for a clean exit made every exit race a rollback.
        const bool done = cv_.wait_until(lk, deadline, [&] {
            return slot->acked.load(std::memory_order_acquire) != 0 ||
                   slot->alive.load(std::memory_order_acquire) == 0;
        });
        lk.unlock();
        if (!done) {
            stat_timeouts_.fetch_add(1, std::memory_order_relaxed);
            // The rolled-back freeze records its FULL window (docs/
            // infrastructure/06 section 5) before the thaw: emitted
            // directly with pending left false so thaw_all below does not
            // double-emit. Generation = the would-be generation (the
            // freeze never completed).
            {
                const uint64_t now = infra::mono_clock_ns();
                std::lock_guard<std::mutex> emit_lk(mu_);
                emit_freeze_locked(now - ttsp_t0, now - ttsp_t0,
                                   static_cast<uint32_t>(alive_ids.size()),
                                   static_cast<uint8_t>(kind), true,
                                   freeze_generation_ + 1);
            }
            // Roll the freeze back before failing loudly (Rule 81).
            (void)thaw_all();
            return support::fail(
                support::ErrorCode::RuntimeError,
                "threading: handshake timeout waiting for thread " +
                    std::to_string(id) + " (Rule 81: bounded, fails loudly)");
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    ++freeze_generation_;
    stat_freezes_.fetch_add(1, std::memory_order_relaxed);
    // TTSP telemetry (docs/infrastructure/06 section 5): the event is
    // RECORDED when the body completes (thaw_all) — the pending fields
    // carry the ttsp (arm -> last acknowledge) and the body's start.
    const uint64_t now = infra::mono_clock_ns();
    freeze_ttsp_ns_ = now - ttsp_t0;
    freeze_start_ns_ = now;
    freeze_threads_ = static_cast<uint32_t>(alive_ids.size());
    freeze_cause_ = static_cast<uint8_t>(kind);
    freeze_event_pending_ = true;
    return freeze_generation_;
}

// CEP:WHAT: Disarms the page and releases every thread; clears the frozen flag.
// CEP:WHY: Runs even when the protect flip fails (the word clears first).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Disarm degradation is reported by disarm() itself.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One disarm + release-all.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> HandshakeManager::thaw_all() {
    if (!platform_supported()) return platform_fail();
    // Disarm clears the word even when the protect flip fails and degrades
    // — waiters unblock either way.
    (void)SafepointPollingPage::disarm();
    if (auto r = release(kAllHandshakeThreads); !r) return r;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // The Freeze event completes here: body = last acknowledge -> thaw
        // (docs/infrastructure/06 section 5).
        if (freeze_event_pending_) {
            emit_freeze_locked(freeze_ttsp_ns_,
                               infra::mono_clock_ns() - freeze_start_ns_,
                               freeze_threads_, freeze_cause_, false,
                               freeze_generation_);
            freeze_event_pending_ = false;
        }
        frozen_.store(false, std::memory_order_release);
    }
    cv_.notify_all();
    return support::ok();
}

// CEP:WHAT: Observability: is a request pending for this id?
// CEP:WHY: Unknown ids report false (never guess).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — an atomic load.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One atomic load.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
bool HandshakeManager::pending(uint64_t tid) const noexcept {
    if (tid == kAllHandshakeThreads) return false;
    const Slot* slot = nullptr;
    if (tid == kAnonymousId) {
        slot = &slots_[0];
    } else if (tid >= 1 && tid - 1 < kMaxThreads) {
        slot = &slots_[tid - 1];
    }
    if (slot == nullptr) return false;
    return slot->pending.load(std::memory_order_acquire) != 0;
}

// CEP:WHAT: Observability: is the manager inside a freeze window?
// CEP:WHY: Relaxed view — the waiters are the synchronization points.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — an atomic load.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One atomic load.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
bool HandshakeManager::frozen() const noexcept {
    // frozen_ is written under mu_; readers accept the relaxed view
    // (observability only — the waiters are the synchronization points).
    return frozen_.load(std::memory_order_acquire);
}

// CEP:WHAT: Observability counters (requests/freezes/timeouts) (Rule 120).
// CEP:WHY: Freeze health is spellable, never guessed.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — atomic loads.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. Three atomic loads.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
HandshakeManager::Stats HandshakeManager::stats() const noexcept {
    Stats s;
    s.requests = stat_requests_.load(std::memory_order_relaxed);
    s.freezes = stat_freezes_.load(std::memory_order_relaxed);
    s.timeouts = stat_timeouts_.load(std::memory_order_relaxed);
    return s;
}

// CEP:WHAT: Emits one Freeze safepoint event into the configured TTSP log
//           (no-op without a sink). Called under mu_; the rollback path
//           passes its explicit fields, the thaw path the pending ones.
// CEP:WHY: docs/infrastructure/06-observability.md section 5 — the p99
//           attribution source (cause, TTSP, pause body).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the log never fails the handshake (telemetry
//           degradation is the documented cost, never a runtime error).
// CEP:ASSUMES: mu_ is held by the caller; the sink outlives the manager
//           while set (the caller owns the pointer contract).
// CEP:COST: @cold. One ring push.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 11-13.
void HandshakeManager::emit_freeze_locked(uint64_t ttsp_ns, uint64_t body_ns,
                                          uint32_t threads, uint8_t cause,
                                          bool timed_out,
                                          uint64_t generation) {
    if (safepoint_log_ == nullptr) return;
    SafepointEvent event;
    event.kind = SafepointEvent::Kind::Freeze;
    event.cause = cause;
    event.ttsp_ns = ttsp_ns;
    event.body_ns = body_ns;
    event.threads = threads;
    event.timed_out = timed_out;
    event.generation = generation;
    safepoint_log_->record(event);
}

// ---- GuardPageSuspender -----------------------------------------------------

namespace {

struct GuardRegion {
    std::atomic<uint32_t> active{0};
    unsigned char* base = nullptr;
    size_t bytes = 0;
    GuardPageSuspender::Record* rec = nullptr;
};

struct GuardState {
    // CEP:WHAT: Process-wide guard-region registry + handler disposition
    //           state. sched_mu serializes arm/disarm/enable/disable and
    //           ensure_installed; the trap handler NEVER takes it (it
    //           scans the atomic region flags directly).
    // CEP:WHY: One registry serves every fiber and bare-thread park owner.
    // CEP:CLASS: CEP-1
    // CEP:STATUS: complete
    // CEP:FAILURE: none — a static accessor (guard_state()).
    // CEP:ASSUMES: The module-level block covers the shared contracts.
    // CEP:COST: @cold. One static access.
    // CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
    //           infrastructure/04 section 10).
    std::mutex sched_mu;
    GuardRegion regions[GuardPageSuspender::kMaxRegions];
    bool handler_enabled = false;
    struct sigaction old_sa{};
    bool old_valid = false;
};

// CEP:WHAT: Process-wide guard-region registry singleton.
// CEP:WHY: One registry serves every fiber and bare-thread park owner.
// CEP:STATUS: complete
// CEP:FAILURE: none — a static accessor.
// CEP:ASSUMES: The module-level block covers the shared contracts.
// CEP:COST: @cold. One static access.
// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
GuardState& guard_state() noexcept {
    static GuardState s;
    return s;
}

// The altstack is PER-THREAD (sigaltstack is task state): each thread that
// can take a guard-page trap installs its own once.
// CEP:WHAT: This thread's 64 KiB trap altstack + installed flag.
// CEP:WHY: sigaltstack is per-task; 64 KiB survives sanitizer interceptor
//           frames (SIGSTKSZ*2 does not).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: an uninstalled thread's faults hit the host handler.
// CEP:ASSUMES: The module-level block covers the shared contracts.
// CEP:COST: @cold. One vector allocation per thread lifetime.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
thread_local std::vector<unsigned char> t_altstack;
thread_local bool t_altstack_installed = false;

// Installs the thread's altstack when trap suspension is enabled. Called
// from enable_trap_suspension (the calling thread) and from every pool
// worker at startup.
// CEP:WHAT: Installs THIS thread's 64 KiB trap altstack when suspension is enabled.
// CEP:WHY: sigaltstack is per-task; a guard trap on a thread without one cannot deliver.
// CEP:STATUS: complete
// CEP:FAILURE: sigaltstack failure leaves the thread uninstalled (faults then hit the host handler).
// CEP:ASSUMES: The module-level block covers the shared contracts.
// CEP:COST: @cold. One sigaltstack per thread lifetime.
// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
void ensure_thread_altstack() noexcept {
    if (t_altstack_installed || !guard_state().handler_enabled) return;
    // 64 KiB: sanitizer builds run fat interceptor frames for TLS access
    // and the handler itself copies the xsave area — SIGSTKSZ*2 is not
    // enough under instrumentation.
    const size_t sz = 64 * 1024;
    t_altstack.assign(sz, 0);
    stack_t ss{};
    ss.ss_sp = t_altstack.data();
    ss.ss_size = t_altstack.size();
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) == 0) {
        t_altstack_installed = true;
    } else {
        t_altstack.clear();
    }
}

// CEP:WHAT: Finds the armed region containing an address (handler path).
// CEP:WHY: Bounded linear scan over atomic flags — no locks in the handler.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a pure lookup.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One bounded scan.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
GuardRegion* region_for_addr(void* addr) noexcept {
    GuardState& g = guard_state();
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    for (size_t i = 0; i < GuardPageSuspender::kMaxRegions; ++i) {
        GuardRegion& r = g.regions[i];
        if (r.active.load(std::memory_order_acquire) == 0) continue;
        const uintptr_t lo = reinterpret_cast<uintptr_t>(r.base);
        if (a >= lo && a < lo + r.bytes) return &r;
    }
    return nullptr;
}

// CEP:WHAT: Finds the armed region with an exact base (disarm/resume paths).
// CEP:WHY: arm() aligns outward, so the base is canonical.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a pure lookup.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One bounded scan.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
GuardRegion* region_for_base(unsigned char* base) noexcept {
    GuardState& g = guard_state();
    for (size_t i = 0; i < GuardPageSuspender::kMaxRegions; ++i) {
        GuardRegion& r = g.regions[i];
        if (r.active.load(std::memory_order_acquire) == 0) continue;
        if (r.base == base) return &r;
    }
    return nullptr;
}

// CEP:WHAT: Finds a free registry slot for arm().
// CEP:WHY: Capacity is bounded (Rule 28); exhaustion rejects by name.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a pure scan.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One bounded scan.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
GuardRegion* free_region() noexcept {
    GuardState& g = guard_state();
    for (size_t i = 0; i < GuardPageSuspender::kMaxRegions; ++i) {
        if (g.regions[i].active.load(std::memory_order_acquire) == 0) {
            return &g.regions[i];
        }
    }
    return nullptr;
}

// Capture the interrupted context into the record: ucontext_t copied, FP
// state copied out of the signal frame (bounded by THIS thread's altstack
// end and the 4096-byte buffer), SIGSEGV unblocked in the saved mask,
// uc_link cleared. Async-signal-safe: memcpy + arithmetic only.
// CEP:WHAT: Copies the interrupted context + FP state into the record for resume.
// CEP:WHY: The FP area is bounded by the altstack end; SIGSEGV unblocked in the mask.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — memcpy + arithmetic (async-signal-safe).
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. Two bounded memcpys.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
void capture_context(GuardPageSuspender::Record* rec, void* uctx) noexcept {
    memcpy(rec->uc_buf, uctx, sizeof(ucontext_t));
    auto* uc = reinterpret_cast<ucontext_t*>(rec->uc_buf);
    if (uc->uc_mcontext.fpregs != nullptr) {
        // The FP area lives inside THIS delivery's signal frame, which the
        // kernel placed at the TOP of this thread's altstack (SA_ONSTACK):
        // bound the copy by the altstack end (the frame grows downward, so
        // fpregs + 4 KiB can cross it).
        const uintptr_t fp = reinterpret_cast<uintptr_t>(uc->uc_mcontext.fpregs);
        const uintptr_t alt_lo =
            reinterpret_cast<uintptr_t>(t_altstack.data());
        const uintptr_t alt_hi = alt_lo + t_altstack.size();
        size_t bytes = GuardPageSuspender::kFpStateBytes;
        if (fp >= alt_lo && fp < alt_hi) {
            const size_t room = static_cast<size_t>(alt_hi - fp);
            if (room < bytes) bytes = room;
        }
        memcpy(rec->fp_buf, reinterpret_cast<const void*>(fp), bytes);
        uc->uc_mcontext.fpregs = reinterpret_cast<fpregset_t>(rec->fp_buf);
    }
    uc->uc_link = nullptr;
    sigdelset(&uc->uc_sigmask, SIGSEGV);
    rec->captured_valid = true;
}

}  // namespace

// CEP:WHAT: Observability: is the chaining SIGSEGV disposition installed?
// CEP:WHY: Tests assert the enable/disable lifecycle through it.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a flag read.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One flag read.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
bool GuardPageSuspender::trap_suspension_enabled() noexcept {
    return guard_state().handler_enabled;
}

// CEP:WHAT: Re-asserts our SIGSEGV disposition when a host displaced it.
// CEP:WHY: Sanitizer runtimes re-install their handlers per thread start.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a no-op when the disposition is already ours.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One sigaction probe (+ re-install when displaced).
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
void GuardPageSuspender::ensure_installed() noexcept {
    GuardState& g = guard_state();
    if (!g.handler_enabled) return;
    // Serialized against enable/disable and concurrent worker startups
    // (M6-b review M8): two threads probing-then-installing concurrently
    // could capture OUR handler as the chain target — every foreign fault
    // would then recurse into ourselves until the altstack dies.
    std::lock_guard<std::mutex> lk(g.sched_mu);
    struct sigaction cur{};
    if (sigaction(SIGSEGV, nullptr, &cur) != 0) return;
    if (cur.sa_sigaction ==
        reinterpret_cast<decltype(cur.sa_sigaction)>(
            &GuardPageSuspender::trap_signal_handler)) {
        return;  // already ours
    }
    // The displaced handler becomes the chain target for foreign faults.
    struct sigaction sa{};
    sa.sa_sigaction = &GuardPageSuspender::trap_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g.old_sa) == 0) {
        g.old_valid = true;
        ensure_thread_altstack();
    }
}

// CEP:WHAT: Installs the chaining SIGSEGV disposition over the previous one.
// CEP:WHY: Foreign faults chain (direct call for SA_SIGINFO, restore for default).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: sigaltstack/sigaction failure -> named rejects, state unchanged.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One sigaction + one altstack install.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> GuardPageSuspender::enable_trap_suspension() {
    if (!platform_supported()) return platform_fail();
    GuardState& g = guard_state();
    std::lock_guard<std::mutex> lk(g.sched_mu);
    if (g.handler_enabled) return support::ok();  // idempotent
    struct sigaction sa{};
    sa.sa_sigaction = &GuardPageSuspender::trap_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g.old_sa) != 0) {
        return support::fail(support::ErrorCode::RuntimeError,
                             "threading: sigaction(SIGSEGV) failed (errno " +
                                 std::to_string(errno) + ")");
    }
    g.old_valid = true;
    g.handler_enabled = true;
    ensure_thread_altstack();  // the calling thread
    return support::ok();
}

// CEP:WHAT: Restores the previous disposition (enable's inverse).
// CEP:WHY: Idempotence: a double disable rejects by name.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Not-enabled -> InvalidState named reject.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One sigaction restore.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> GuardPageSuspender::disable_trap_suspension() {
    if (!platform_supported()) return platform_fail();
    GuardState& g = guard_state();
    std::lock_guard<std::mutex> lk(g.sched_mu);
    if (!g.handler_enabled) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: trap suspension not enabled");
    }
    if (g.old_valid) {
        (void)sigaction(SIGSEGV, &g.old_sa, nullptr);
    } else {
        signal(SIGSEGV, SIG_DFL);
    }
    g.handler_enabled = false;
    return support::ok();
}

// CEP:WHAT: Registers [base, base+size) (page-aligned outward) as a guard region.
// CEP:WHY: Overlaps reject; the registry is bounded (Rule 28).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Zero size / null args / overlap / registry full / mprotect -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One mprotect + one registry slot.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> GuardPageSuspender::arm(void* base, size_t size,
                                              Record* record) {
    if (!platform_supported()) return platform_fail();
    if (base == nullptr || size == 0 || record == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: guard arm needs base, size, record");
    }
    GuardState& g = guard_state();
    std::lock_guard<std::mutex> lk(g.sched_mu);
    const size_t ps = os_page_size();
    const uintptr_t lo = align_down(reinterpret_cast<uintptr_t>(base), ps);
    const uintptr_t hi = align_up(reinterpret_cast<uintptr_t>(base) + size, ps);
    for (size_t i = 0; i < kMaxRegions; ++i) {
        const GuardRegion& r = g.regions[i];
        if (r.active.load(std::memory_order_acquire) == 0) continue;
        const uintptr_t rlo = reinterpret_cast<uintptr_t>(r.base);
        if (lo < rlo + r.bytes && rlo < hi) {
            return support::fail(support::ErrorCode::InvalidState,
                                 "threading: guard region overlaps an armed "
                                 "region");
        }
    }
    GuardRegion* slot = free_region();
    if (slot == nullptr) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "threading: guard region registry full (1024)");
    }
    // Publish the registry entry — fields AND the active flag — BEFORE the
    // protect (M6-b review M7 + verification round): a descent crossing the
    // page inside the protect window must find its record; publishing after
    // (or gating on a flag stored after the protect) leaves a window where
    // the suspension fault classifies as foreign and chains (process death
    // instead of suspension). No fault can consume the entry while the page
    // is still readable, so the early activation is safe; arms are
    // serialized under sched_mu, and the handler reads base/bytes/rec only
    // after acquiring `active` (release/acquire pairing).
    slot->base = reinterpret_cast<unsigned char*>(lo);
    slot->bytes = hi - lo;
    slot->rec = record;
    slot->active.store(1, std::memory_order_release);
    if (mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_NONE) != 0) {
        // Rollback: the page is still readable — no fault could have used
        // the entry; deactivate before clearing the fields.
        slot->active.store(0, std::memory_order_release);
        slot->rec = nullptr;
        slot->bytes = 0;
        slot->base = nullptr;
        return support::fail(support::ErrorCode::RuntimeError,
                             "threading: guard arm mprotect failed (errno " +
                                 std::to_string(errno) + ")");
    }
    return support::ok();
}

// CEP:WHAT: Arms the single page strictly below the page containing sp_hint.
// CEP:WHY: The descent backstop: the next push below the basis faults.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Null args / page underflow / overlap -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One aligned subtraction + arm.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> GuardPageSuspender::arm_sp_patch(Record* record,
                                                       void* sp_hint) {
    if (!platform_supported()) return platform_fail();
    if (record == nullptr || sp_hint == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: arm_sp_patch needs record and sp");
    }
    const size_t ps = os_page_size();
    const uintptr_t below =
        align_down(reinterpret_cast<uintptr_t>(sp_hint), ps) - ps;
    if (below == 0) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: sp patch page underflow");
    }
    return arm(reinterpret_cast<void*>(below), ps, record);
}

// CEP:WHAT: Disarms the region armed FOR this record (find by rec).
// CEP:WHY: Reclaiming a fiber reclaims its PATCH region too — the low guard
//           has its own base, but a still-armed patch would leak a dangling
//           record into the registry (M6-b review round 2).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Nothing armed for the record -> named reject.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One bounded registry scan + one mprotect.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> GuardPageSuspender::disarm_record(Record* record) {
    if (!platform_supported()) return platform_fail();
    if (record == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: guard disarm_record needs a record");
    }
    GuardState& g = guard_state();
    std::lock_guard<std::mutex> lk(g.sched_mu);
    // ALL regions for the record (verification round NEW-3: repeated
    // suspend_fiber re-arms could leave more than one patch; a first-match
    // scan would leak the rest — stale PROT_NONE pages over reclaimed
    // memory). Serialized under sched_mu, so no concurrent arm interleaves.
    bool disarmed_any = false;
    for (size_t i = 0; i < kMaxRegions; ++i) {
        GuardRegion& r = g.regions[i];
        if (r.active.load(std::memory_order_acquire) == 0) continue;
        if (r.rec != record) continue;
        if (mprotect(r.base, r.bytes, PROT_READ | PROT_WRITE) != 0) {
            return support::fail(support::ErrorCode::RuntimeError,
                                 "threading: guard disarm_record mprotect "
                                 "failed (errno " +
                                     std::to_string(errno) + ")");
        }
        r.active.store(0, std::memory_order_release);
        disarmed_any = true;
    }
    if (!disarmed_any) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: no armed guard region for the record");
    }
    return support::ok();
}

// CEP:WHAT: Makes a guard region readable again, then deactivates its registry slot.
// CEP:WHY: Readable FIRST: a racing handler either finds the entry or chains safely.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: No region at base / mprotect failure -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One mprotect + one flag store.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> GuardPageSuspender::disarm(void* base) {
    if (!platform_supported()) return platform_fail();
    if (base == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: guard disarm needs a base");
    }
    GuardState& g = guard_state();
    std::lock_guard<std::mutex> lk(g.sched_mu);
    const size_t ps = os_page_size();
    const uintptr_t lo = align_down(reinterpret_cast<uintptr_t>(base), ps);
    GuardRegion* slot = region_for_base(reinterpret_cast<unsigned char*>(lo));
    if (slot == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: no armed guard region at base");
    }
    // Readable FIRST, then deactivate: a handler racing the disarm either
    // finds the entry (resume re-executes on a readable page — fine) or
    // chains (the re-executed access succeeds — fine).
    if (mprotect(slot->base, slot->bytes, PROT_READ | PROT_WRITE) != 0) {
        return support::fail(support::ErrorCode::RuntimeError,
                             "threading: guard disarm mprotect failed "
                             "(errno " +
                                 std::to_string(errno) + ")");
    }
    slot->active.store(0, std::memory_order_release);
    return support::ok();
}

// CEP:WHAT: Resumes a suspended record: disarm the trapped region, swapcontext back.
// CEP:WHY: The faulting access re-executes on a readable page; the low guard stays armed.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Non-suspended record / mprotect failure -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One registry scan + swapcontext.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> GuardPageSuspender::resume(Record* record,
                                                 ucontext_t* comeback) {
    if (!platform_supported()) return platform_fail();
    if (record == nullptr || comeback == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: resume needs record and comeback");
    }
    if (record->state.load(std::memory_order_acquire) != kStateSuspended ||
        !record->captured_valid) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: resume of a record that is not "
                             "suspended");
    }
    {
        GuardState& g = guard_state();
        std::lock_guard<std::mutex> lk(g.sched_mu);
        // Disarm EXACTLY the region that trapped (a fiber also owns its
        // permanent low stack-guard page — that one stays armed).
        GuardRegion* r = record->fault_region != nullptr
                             ? region_for_base(record->fault_region)
                             : nullptr;
        if (r != nullptr) {
            if (mprotect(r->base, r->bytes, PROT_READ | PROT_WRITE) != 0) {
                return support::fail(
                    support::ErrorCode::RuntimeError,
                    "threading: resume disarm mprotect failed (errno " +
                        std::to_string(errno) + ")");
            }
            r->active.store(0, std::memory_order_release);
            record->fault_region = nullptr;
        }
    }
    record->state.store(kStateRunning, std::memory_order_release);
    // The record re-executes the faulting access; the faulting page is
    // readable now. Exactly one live context per record (state machine).
    swapcontext(comeback, reinterpret_cast<ucontext_t*>(record->uc_buf));
    return support::ok();
}

// CEP:WHAT: Test/observability: is any armed region containing this address?
// CEP:WHY: Region arithmetic is page-aligned outward (arm).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a bounded registry scan.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One bounded scan.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
bool GuardPageSuspender::armed_for(const void* addr) noexcept {
    return region_for_addr(const_cast<void*>(addr)) != nullptr;
}

// CEP:WHAT: The one SIGSEGV disposition: suspend on guard regions, chain foreign faults.
// CEP:WHY: Async-signal-safe body: atomics, memcpy, siglongjmp; no locks, no TLS.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Foreign faults chain to the previous disposition (direct call or restore).
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One bounded registry scan + context capture.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
void GuardPageSuspender::trap_signal_handler(int signo, siginfo_t* info,
                                             void* uctx) noexcept {
    (void)signo;
    GuardRegion* r = region_for_addr(info->si_addr);
    if (r == nullptr || r->rec == nullptr) {
        // Foreign fault. Restoring the previous disposition and returning
        // would PERMANENTLY uninstall this handler the first time a
        // previous handler returns without dying (sanitizer reports do) —
        // every later suspension fault would then go to it instead. If the
        // previous disposition is a SA_SIGINFO handler, call it DIRECTLY
        // and stay installed; only a default disposition uninstalls (the
        // re-executed instruction refaults into it and dies — correct for
        // genuine crashes). No stdio here (M6-b review M9: snprintf may
        // malloc — the faulting thread can hold the allocator lock; the
        // sanitizer report IS the diagnostic).
        GuardState& g = guard_state();
        if (g.old_valid && (g.old_sa.sa_flags & SA_SIGINFO) != 0 &&
            g.old_sa.sa_sigaction != nullptr &&
            g.old_sa.sa_sigaction !=
                reinterpret_cast<decltype(g.old_sa.sa_sigaction)>(
                    &GuardPageSuspender::trap_signal_handler)) {
            g.old_sa.sa_sigaction(signo, info, uctx);
            return;
        }
        if (g.old_valid) (void)sigaction(SIGSEGV, &g.old_sa, nullptr);
        return;
    }
    Record* rec = r->rec;
    if (rec->lifetime_base != nullptr && r->base == rec->lifetime_base) {
        // Stack overflow into a fiber's lifetime low guard (M6-b review
        // B3): FAIL the owner by name. Suspending here would let resume()
        // disarm the lifetime guard and re-execute the overflowing access
        // on a readable page — silent corruption below the mapping. The
        // guard STAYS armed; no fault_region is recorded, resume has
        // nothing to disarm. run_on_current turns kStateFaulted into a
        // named fiber failure that join() propagates. Suspension patches
        // armed over the SAME record (different bases) keep suspending —
        // only the exact lifetime base classifies as overflow.
        rec->state.store(kStateFaulted, std::memory_order_release);
        siglongjmp(rec->park, 1);
    }
    rec->fault_region = r->base;  // resume disarms exactly this region
    capture_context(rec, uctx);
    rec->state.store(kStateSuspended, std::memory_order_release);
    siglongjmp(rec->park, 1);
}

// ---- WorkStealingPool -------------------------------------------------------

namespace {

enum : uint32_t {
    // CEP:WHAT: Fiber scheduler states (Rule 120: dumps spell states).
    // CEP:WHY: The observability contract maps ids to names one-to-one.
    // CEP:CLASS: CEP-1
    // CEP:STATUS: complete
    // CEP:FAILURE: none — constants.
    // CEP:ASSUMES: The module-level block covers the shared contracts.
    // CEP:COST: compile-time.
    // CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
    //           infrastructure/04 section 10).
    kFibReady = 0,
    kFibRunning = 1,
    kFibSuspended = 2,
    kFibDone = 3,
};

// Thread-current scheduling slots (one fiber runs per OS thread at a time).
// CEP:WHAT: The current thread's scheduler slots (pool/worker/loop/fiber
//           + poll counter).
// CEP:WHY: resume_fiber saves/restores them; the hook reads the fiber.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — slot storage.
// CEP:ASSUMES: The module-level block covers the shared contracts; the
//           trap handler does not read these (no TLS in the handler).
// CEP:COST: @cold. One TLS access per use.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
thread_local WorkStealingPool* t_pool = nullptr;
thread_local WorkStealingPool::Worker* t_worker = nullptr;
thread_local ucontext_t* t_loop = nullptr;
thread_local WorkStealingPool::Fiber* t_fiber = nullptr;
thread_local uint32_t t_poll_count = 0;

}  // namespace

struct WorkStealingPool::Fiber {
    uint64_t id = 0;
    Entry entry = nullptr;
    void* user = nullptr;
    uint64_t hs_id = 0;  // handshake id once its owner registers it
    std::atomic<uint64_t> last_sp{0};  // the entry trampoline's frame
                                       // address — the suspension-patch
                                       // basis (INSIDE this fiber's own
                                       // mapping, unlike the primed
                                       // context RSP which sits at the
                                       // mapping end)
    std::atomic<uint64_t> patch_base{0};  // the live suspension patch's
                                          // armed base (0 = none); a
                                          // re-arm disarms it first so a
                                          // fiber never accumulates
                                          // patches (verification NEW-3)
    unsigned char* map = nullptr;
    size_t map_bytes = 0;
    void* stack_top = nullptr;
    size_t usable = 0;
    ucontext_t ctx{};
    GuardPageSuspender::Record guard{};
    std::atomic<uint32_t> state{kFibReady};
    support::Result<void> result{};
};

struct WorkStealingPool::Worker {
    uint64_t id = 0;
    WorkStealingPool* pool = nullptr;  // launch back-pointer (stable address)
    pthread_t th{};
    bool started = false;
    ucontext_t loop_ctx{};
    std::mutex mu;
    std::deque<uint64_t> local;
    uint64_t steals = 0;
    uint64_t rng = 0;
};

struct WorkStealingPool::Impl {
    uint32_t n_workers = 0;
    std::vector<std::unique_ptr<Worker>> workers;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<uint64_t> global_ready;
    // Ownership table (mu-protected writers only — spawn push_back may
    // reallocate; every vector ACCESS is under impl->mu).
    std::vector<std::unique_ptr<Fiber>> fibers;
    // Stable publish table for the LOCK-FREE readers (fiber() from
    // fiber_thunk runs on worker threads without mu): fixed capacity
    // (kMaxFibers), allocated once at pool construction, slots published
    // with release stores — a reallocation can never invalidate a reader
    // (the M6-c gates caught the vector-read UAF: spawn's push_back
    // reallocating while fiber_thunk indexed the old array).
    std::unique_ptr<std::atomic<Fiber*>[]> fiber_slots;
    bool stop = false;
    bool degraded = false;
    bool shutdown_done = false;
    HandshakeManager* hs = nullptr;
    SafepointPollingPage* page = nullptr;
    std::vector<vm::Interpreter*> adopted;
    std::atomic<uint64_t> s_spawned{0};
    std::atomic<uint64_t> s_completed{0};
    std::atomic<uint64_t> s_stolen{0};
    std::atomic<uint64_t> s_yields{0};
    std::atomic<uint64_t> s_freeze_polls{0};
    std::atomic<uint64_t> s_run_rejected{0};

// CEP:WHAT: Resolves a fiber id to its record (ids are 1-based vector indexes).
// CEP:WHY: Bounds-checked: unknown ids return nullptr, callers reject by name.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a bounds check.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One bounds check.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
    Fiber* fiber(uint64_t fid) const noexcept {
        if (fid == 0 || fid - 1 >= kMaxFibers) return nullptr;
        // Acquire-load of the publish slot: a spawned fiber's fields are
        // written-before-publish (release in spawn), so a reader that sees
        // the slot sees the fiber ready. Unpublished slots read nullptr.
        return fiber_slots[fid - 1].load(std::memory_order_acquire);
    }

// CEP:WHAT: True when no queued fiber remains (shutdown drain predicate).
// CEP:WHY: Workers exit only when stop is set AND every queue drained.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — small mutex sections.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. N small mutex sections.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
    bool queues_empty() const {
        if (!global_ready.empty()) return false;
        for (const auto& w : workers) {
            std::lock_guard<std::mutex> lk(w->mu);
            if (!w->local.empty()) return false;
        }
        return true;
    }

// CEP:WHAT: Pops the next runnable fiber: local, then global, then steal (random tail).
// CEP:WHY: Stealing rebalances the round-robin spawn placement.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — empty queues return nullptr.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. Up to three small mutex sections.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
    Fiber* pop_runnable(Worker& w) {
        {
            std::lock_guard<std::mutex> lk(w.mu);
            if (!w.local.empty()) {
                uint64_t id = w.local.front();
                w.local.pop_front();
                return fiber(id);
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!global_ready.empty()) {
                uint64_t id = global_ready.front();
                global_ready.pop_front();
                return fiber(id);
            }
        }
        // Randomized stealing from the victim's tail.
        const uint32_t n = n_workers;
        if (n <= 1) return nullptr;
        for (uint32_t tries = 0; tries < n; ++tries) {
            w.rng ^= w.rng << 13;
            w.rng ^= w.rng >> 7;
            w.rng ^= w.rng << 17;
            const uint32_t victim = static_cast<uint32_t>(w.rng % n);
            Worker& v = *workers[victim];
            if (&v == &w) continue;
            std::lock_guard<std::mutex> lk(v.mu);
            if (v.local.empty()) continue;
            const uint64_t id = v.local.back();
            v.local.pop_back();
            ++w.steals;
            s_stolen.fetch_add(1, std::memory_order_relaxed);
            return fiber(id);
        }
        return nullptr;
    }

// CEP:WHAT: Requeues a yielded fiber to the current worker (or the global queue).
// CEP:WHY: M:N rotation: the fiber returns through some worker's loop.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a small mutex push.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One small mutex push + notify.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
    void requeue(Fiber& f) {
        if (t_worker != nullptr) {
            std::lock_guard<std::mutex> lk(t_worker->mu);
            t_worker->local.push_back(f.id);
        } else {
            std::lock_guard<std::mutex> lk(mu);
            global_ready.push_back(f.id);
        }
        cv.notify_all();
    }

// CEP:WHAT: Swaps a fiber in on the CURRENT thread until yield/done/trap.
// CEP:WHY: Fresh sigsetjmp park every swap-in (a stale park would longjmp into a dead frame).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Done / running-elsewhere fibers -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One swapcontext (+ park bookkeeping).
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
    support::Result<WorkStealingPool::RunOutcome> run_on_current(Fiber& f) {
        uint32_t prior = f.state.load(std::memory_order_acquire);
        if (prior == kFibDone) {
            return support::fail(support::ErrorCode::InvalidState,
                                 "threading: fiber already done: " +
                                     std::to_string(f.id));
        }
        if (prior == kFibRunning) {
            return support::fail(support::ErrorCode::InvalidState,
                                 "threading: fiber is running on another "
                                 "thread: " +
                                     std::to_string(f.id));
        }
        // Claim the fiber atomically (M6-b review B2b): the old load-then-
        // store let two runners pass the same window; the CAS from the
        // observed state admits exactly one and names the loser.
        if (!f.state.compare_exchange_strong(prior, kFibRunning,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            return support::fail(support::ErrorCode::InvalidState,
                                 "threading: fiber is running on another "
                                 "thread: " +
                                     std::to_string(f.id));
        }
        t_fiber = &f;
        if (prior != kFibSuspended) {
            // The suspended-resume path leaves the Record's state to
            // GuardPageSuspender::resume (which validates Suspended first).
            f.guard.state.store(GuardPageSuspender::kStateRunning,
                                std::memory_order_release);
        }
        // setjmp/longjmp discipline: locals modified between sigsetjmp and
        // the handler's siglongjmp must be volatile or they read
        // indeterminate after the jump (the worker then mis-classifies the
        // trap and the fiber strands in Running forever).
        volatile bool trapped = false;
        // Fresh park every swap-in (the park point must live on the CURRENT
        // thread's stack — a stale park would siglongjmp into a dead frame).
        if (sigsetjmp(f.guard.park, 1) == 0) {
            ucontext_t* comeback = t_loop;
            if (prior == kFibSuspended) {
                // GuardPageSuspender::resume disarms exactly the region that
                // trapped and swaps into the captured context.
                auto r = GuardPageSuspender::resume(&f.guard, comeback);
                if (!r) {
                    // Resume failed before the swap: restore the suspended
                    // state so a later retry is possible, report by name.
                    f.state.store(kFibSuspended, std::memory_order_release);
                    t_fiber = nullptr;
                    return std::unexpected(std::move(r).error());
                }
            } else {
                swapcontext(comeback, &f.ctx);
            }
        } else {
            *const_cast<volatile bool*>(&trapped) =
                true;  // the handler siglongjmp'd here after capture
        }
        t_fiber = nullptr;
        uint32_t st = f.state.load(std::memory_order_acquire);
        if (trapped) {
            if (f.guard.state.load(std::memory_order_acquire) ==
                GuardPageSuspender::kStateFaulted) {
                // Lifetime low-guard fault (M6-b review B3): the fiber
                // overflowed its stack. Fail it by name — the lifetime
                // guard stays armed, the mapping is reclaimed at retire,
                // and join() propagates the diagnostic.
                f.result = support::fail(
                    support::ErrorCode::RuntimeError,
                    "threading: fiber stack overflow (lifetime guard "
                    "faulted: " +
                        std::to_string(f.id) + ")");
                f.state.store(kFibDone, std::memory_order_release);
                retire(f);
                return RunOutcome::Done;
            }
            // The handler suspended the Record; mirror it on the fiber.
            f.state.store(kFibSuspended, std::memory_order_release);
            st = kFibSuspended;
        }
        if (st == kFibSuspended) return RunOutcome::Suspended;
        if (st == kFibDone) {
            retire(f);
            return RunOutcome::Done;
        }
        // Yielded. The fiber kept its Running mark THROUGH the save (the
        // verification round's NEW-1: publishing Ready before the save let
        // a concurrent resume_fiber claim the fiber and swap into a torn
        // context mid-save — a claimer now sees Running and gets the named
        // reject until this line). The save is complete: publish Ready,
        // re-base the suspension-patch basis at the park (M6-b review M10:
        // the saved context RSP is the fiber's live depth at the yield),
        // and requeue. A concurrent claim between the Ready store and the
        // requeue skips the push (the claimer runs the fiber; no duplicate
        // activation is possible — the CAS gate owns exclusivity).
        const uint64_t rsp =
            static_cast<uint64_t>(f.ctx.uc_mcontext.gregs[REG_RSP]);
        if (rsp != 0) f.last_sp.store(rsp, std::memory_order_release);
        f.state.store(kFibReady, std::memory_order_release);
        requeue(f);
        return RunOutcome::Yielded;
    }

// CEP:WHAT: Frees a done fiber's stack and guard region (the record stays for join).
// CEP:WHY: join() reads the result after Done; the mapping is the only reclaim.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — best-effort disarm+munmap.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One disarm + munmap.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
    void retire(Fiber& f) {
        // Release the handshake slot (M6-b review M6): a done fiber is no
        // longer a mutator; its slot recycles for the next registration.
        if (f.hs_id != 0 && hs != nullptr) {
            (void)hs->unregister_thread(f.hs_id);
            f.hs_id = 0;
        }
        if (f.map != nullptr) {
            // Reclaim BOTH armed regions: the low guard by exact base, then
            // whatever patch remains for the record (a leaked region would
            // keep a dangling record in the registry and poison later arms
            // — M6-b review round 2).
            (void)GuardPageSuspender::disarm(f.map);
            (void)GuardPageSuspender::disarm_record(&f.guard);
            (void)munmap(f.map, f.map_bytes);
            f.map = nullptr;
        }
        f.guard.state.store(GuardPageSuspender::kStateIdle,
                            std::memory_order_release);
    }

// CEP:WHAT: Fiber bootstrap: records the frame basis, runs the entry, publishes Done.
// CEP:WHY: The recorded frame is the suspension-patch basis (inside the mapping).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the entry's own Result is the failure surface.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One frame store + the entry call.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
    void fiber_main(Fiber& f) {
        t_fiber = &f;
        // Record THIS fiber's live frame address: the suspension patch
        // basis. The makecontext-primed RSP equals the mapping END (the
        // stack grows down from it) — "one page below" that is either the
        // page the entry frames occupy or a FOREIGN mapping. The entry's
        // own frame is always inside the mapping and above every deeper
        // descent.
        f.last_sp.store(
            reinterpret_cast<uint64_t>(__builtin_frame_address(0)),
            std::memory_order_release);
        support::Result<void> r = f.entry(f.user);
        f.result = std::move(r);
        f.state.store(kFibDone, std::memory_order_release);
        s_completed.fetch_add(1, std::memory_order_relaxed);
        cv.notify_all();
        ucontext_t* back = t_loop != nullptr ? t_loop : &f.ctx;
        swapcontext(&f.ctx, back);
        // Not reached in normal operation (the swap returns to the scheduler
        // that ran us; a stray return here just idles this context).
    }
};

// ---- pool plumbing (static members: the launch trampolines keep class
// access to the private internals) ------------------------------------------

// CEP:WHAT: makecontext trampoline: reconstructs the fiber id and enters fiber_main.
// CEP:WHY: The primed context carries the id as two int halves (LP64-safe).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a null pool/fiber returns quietly (cannot happen).
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One id reconstruct + dispatch.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
void WorkStealingPool::fiber_thunk(int lo, int hi) noexcept {
    const uint64_t fid =
        (static_cast<uint64_t>(static_cast<uint32_t>(hi)) << 32) |
        static_cast<uint32_t>(lo);
    WorkStealingPool* pool = t_pool;
    if (pool == nullptr || pool->impl_ == nullptr) return;
    Fiber* f = pool->impl_->fiber(fid);
    if (f == nullptr) return;
    pool->impl_->fiber_main(*f);
}

// CEP:WHAT: Worker loop: pop -> run -> park, exiting on stop+drain.
// CEP:WHY: Re-asserts the trap disposition and altstack (host runtimes displace them).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the loop exits on shutdown.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One pop per fiber; condvar park when idle.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
void WorkStealingPool::worker_main(Worker& w) {
    Impl& impl = *w.pool->impl_;
    // Host runtimes (sanitizers) re-install their own SIGSEGV disposition
    // when a thread starts — re-assert ours after it.
    ensure_thread_altstack();
    GuardPageSuspender::ensure_installed();
    t_pool = w.pool;
    t_worker = &w;
    t_loop = &w.loop_ctx;
    t_fiber = nullptr;
    for (;;) {
        Fiber* f = impl.pop_runnable(w);
        if (f == nullptr) {
            std::unique_lock<std::mutex> lk(impl.mu);
            if (impl.stop && impl.queues_empty()) break;
            impl.cv.wait_for(lk, std::chrono::milliseconds(1));
            continue;
        }
        auto r = impl.run_on_current(*f);
        if (!r) {
            // Counted, never silent (M6-b review m4): a rejected swap-in
            // (a stale queue entry losing the CAS claim) is scheduler
            // telemetry, not noise (Rule 120).
            impl.s_run_rejected.fetch_add(1, std::memory_order_relaxed);
        }
    }
    t_worker = nullptr;
    t_loop = nullptr;
    t_fiber = nullptr;
}

// CEP:WHAT: pthread start routine: dispatches to worker_main.
// CEP:WHY: pthreads (error-code API) keep pool construction exception-free.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a trampoline.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One dispatch per worker lifetime.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
void* WorkStealingPool::worker_tramp(void* arg) noexcept {
    auto* w = static_cast<Worker*>(arg);
    worker_main(*w);
    return nullptr;
}

// CEP:WHAT: Starts the worker threads (0 = driver-only mode, no workers).
// CEP:WHY: pthreads keep construction exception-free (Rule 65).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: pthread_create failure -> degraded pool; spawn rejects by name.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. N pthread_creates.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
WorkStealingPool::WorkStealingPool(uint32_t os_threads) : impl_(nullptr) {
    // No exception path: workers are pthreads (error-code API). A failed
    // worker leaves a degraded pool that rejects spawn by name.
    impl_ = new (std::nothrow) Impl();
    if (impl_ == nullptr) return;  // host OOM: destructor and spawn reject
    Impl& impl = *impl_;
    // The publish slot table is a 512 KiB fixed allocation (M6-c review
    // R2-8): a throwing new[] inside a nothrow-new-constructed Impl would
    // escape the pool constructor (the file's Rule 65 posture: error-code
    // API, no exceptions). Nothrow + the existing degraded route.
    impl.fiber_slots.reset(new (std::nothrow) std::atomic<Fiber*>[kMaxFibers]{});
    if (impl.fiber_slots == nullptr) {
        impl.degraded = true;
        return;
    }
    if (os_threads == 0) {
        // Driver-only mode: a VALID pool with no workers — fibers are run
        // by the embedder through resume_fiber (deterministic driving; the
        // suspension paths are then exercised on the caller's thread, with
        // the caller's own trap disposition and altstack).
        impl.n_workers = 0;
        return;
    }
    impl.n_workers = os_threads;
    for (uint32_t i = 0; i < os_threads; ++i) {
        auto w = std::make_unique<Worker>();
        w->id = i + 1;
        w->pool = this;
        w->rng = 0x9E37'79B9'7F4A'7C15ull ^ (uint64_t{i + 1} << 32) ^
                 reinterpret_cast<uint64_t>(w.get());
        impl.workers.push_back(std::move(w));
        Worker* wp = impl.workers.back().get();
        const int rc = pthread_create(&wp->th, nullptr, worker_tramp, wp);
        if (rc == 0) {
            wp->started = true;
        } else {
            impl.workers.pop_back();  // never started; nothing to join
            impl.n_workers = static_cast<uint32_t>(impl.workers.size());
            impl.degraded = impl.n_workers == 0;
            break;
        }
    }
}

// ---- WorkStealingPool public API --------------------------------------------

// CEP:WHAT: Creates a guard-page fiber stack, primes its context, and queues it.
// CEP:WHY: The low guard page lives for the fiber's lifetime (Rule 82 hygiene).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Shutdown/degraded/null-entry/small-stack/full-table/mmap/getcontext -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One mmap + makecontext + queue push (table lock held).
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<uint64_t> WorkStealingPool::spawn(GreenThread& thread) {
    Impl* impl = impl_;
    if (impl == nullptr) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "threading: pool allocation failed");
    }
    if (impl->degraded) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: pool degraded (worker construction "
                             "failed)");
    }
    if (thread.entry == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: spawn needs an entry function");
    }
    const size_t stack =
        thread.stack_size != 0 ? thread.stack_size : kDefaultStackBytes;
    if (stack < kMinStackBytes) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: stack below the 64 KiB minimum");
    }
    // v1: the whole spawn holds the table lock — a cold path (one mmap, one
    // makecontext, one queue push); scalability upgrade tracked in roadmap.
    std::lock_guard<std::mutex> lk(impl->mu);
    if (impl->stop) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: spawn after shutdown");
    }
    // Rejected during a freeze window (M6-b review M5): the fiber would be
    // queued and picked up inside the "stopped" world, and its handshake
    // registration (below) is frozen-window-rejected anyway.
    if (impl->hs != nullptr && impl->hs->frozen()) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: spawn during a freeze window");
    }
    if (impl->fibers.size() >= kMaxFibers) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "threading: fiber table full (65536)");
    }
    const size_t ps = os_page_size();
    auto f = std::make_unique<Fiber>();
    f->map = static_cast<unsigned char*>(
        mmap(nullptr, stack + ps, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (f->map == MAP_FAILED) {
        f->map = nullptr;
        return support::fail(support::ErrorCode::OutOfMemory,
                             "threading: fiber stack mmap failed (errno " +
                                 std::to_string(errno) + ")");
    }
    f->map_bytes = stack + ps;
    f->usable = stack;
    f->stack_top = f->map + ps;
    f->entry = thread.entry;
    f->user = thread.user;
    // Permanent low guard page for the fiber's lifetime (Rule 82 hygiene:
    // a runaway guest stack faults instead of corrupting foreign memory).
    if (auto r = GuardPageSuspender::arm(f->map, ps, &f->guard); !r) {
        (void)munmap(f->map, f->map_bytes);
        f->map = nullptr;
        return std::unexpected(std::move(r).error());
    }
    const uint64_t fid = impl->fibers.size() + 1;
    if (getcontext(&f->ctx) != 0) {
        (void)GuardPageSuspender::disarm(f->map);
        (void)munmap(f->map, f->map_bytes);
        return support::fail(support::ErrorCode::RuntimeError,
                             "threading: getcontext failed (errno " +
                                 std::to_string(errno) + ")");
    }
    f->ctx.uc_stack.ss_sp = f->stack_top;
    f->ctx.uc_stack.ss_size = f->usable;
    f->ctx.uc_link = nullptr;
    makecontext(&f->ctx, reinterpret_cast<void (*)()>(fiber_thunk), 2,
                static_cast<int>(static_cast<uint32_t>(fid)),
                static_cast<int>(static_cast<uint32_t>(fid >> 32)));
    f->id = fid;
    f->guard.token = fid;
    // The low page is the fiber's LIFETIME overflow backstop (M6-b review
    // B3): a fault AT THE MAP BASE fails the fiber by name — it is never
    // suspended, never disarmed by resume. Patches armed over the same
    // record keep suspending (classification is by exact base).
    f->guard.lifetime_base = f->map;
    // Register with the handshake when one is bound (M6-b review M6): the
    // fiber's polls then acknowledge a freeze for real. A FREEZE-WINDOW
    // rejection rejects the spawn (verification round NEW-2: degrading it
    // silently would queue an unregistered fiber inside the stopped world
    // — M5's exact outcome via a race); a full table degrades to hs_id 0
    // (the fiber never acks and the freeze does not wait on it —
    // documented degrade).
    if (impl->hs != nullptr) {
        auto hs_id = impl->hs->register_thread();
        if (!hs_id) {
            if (impl->hs->frozen()) {
                return std::unexpected(std::move(hs_id).error());
            }
        } else {
            f->hs_id = *hs_id;
        }
    }
    // Round-robin spawn placement (creates the imbalance stealing rebalances).
    const uint32_t rr = impl->workers.empty()
                            ? 0
                            : impl->s_spawned.load(std::memory_order_relaxed) %
                                    static_cast<uint32_t>(impl->workers.size());
    impl->fibers.push_back(std::move(f));
    // Publish for the lock-free readers (release: the vector move into the
    // slot is the last write; fiber() acquires). Under impl->mu, so slot
    // writes are serialized.
    impl->fiber_slots[fid - 1].store(impl->fibers[fid - 1].get(),
                                     std::memory_order_release);
    if (!impl->workers.empty()) {
        // Under the worker's own mutex (M6-b review B1): workers pop the
        // front and stealers pop the back of this same deque — a push
        // holding only impl->mu raced them (impl->mu -> w.mu is the
        // established lock order; no path takes them the other way).
        std::lock_guard<std::mutex> lk(impl->workers[rr]->mu);
        impl->workers[rr]->local.push_back(fid);
    } else {
        impl->global_ready.push_back(fid);  // driver-only: resume_fiber pops
    }
    impl->cv.notify_all();
    impl->s_spawned.fetch_add(1, std::memory_order_relaxed);
    thread.id = fid;
    thread.stack_base = impl->fibers[fid - 1]->stack_top;
    thread.state = kFibReady;
    return fid;
}

// CEP:WHAT: Convenience overload forwarding to the GreenThread form.
// CEP:WHY: Same contract as the GreenThread form.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Same named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One struct fill + forward.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<uint64_t> WorkStealingPool::spawn(Entry entry, void* user,
                                                  size_t stack_bytes) {
    GreenThread t;
    t.entry = entry;
    t.user = user;
    t.stack_size = stack_bytes;
    return spawn(t);
}

// CEP:WHAT: Cooperative yield: requeue Ready and swapcontext back to the scheduler.
// CEP:WHY: The M:N rotation point (also the poll-hook rotation).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Off-fiber / off-scheduler contexts -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One swapcontext + small queue push.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> WorkStealingPool::yield_current() {
    Impl* impl = impl_;
    Fiber* f = t_fiber;
    if (f == nullptr || impl == nullptr) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: yield off a fiber context "
                             "(NotFiberContext)");
    }
    if (t_loop == nullptr) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: yield outside a scheduler context");
    }
    impl->s_yields.fetch_add(1, std::memory_order_relaxed);
    // The fiber KEEPS its Running mark through the save (verification
    // round NEW-1): a concurrent resume_fiber sees Running and gets the
    // named reject until run_on_current publishes Ready post-save.
    swapcontext(&f->ctx, t_loop);
    return support::ok();
}

// CEP:WHAT: Arms the descent backstop page below the fiber's recorded frame.
// CEP:WHY: The basis lives inside the fiber's own mapping (never the primed END).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Unknown/done/unstarted fibers -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One registry arm.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> WorkStealingPool::suspend_fiber(uint64_t fid) {
    Impl* impl = impl_;
    if (impl == nullptr) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "threading: pool allocation failed");
    }
    Fiber* f = impl->fiber(fid);
    if (f == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: unknown fiber id: " +
                                 std::to_string(fid));
    }
    const uint32_t st = f->state.load(std::memory_order_acquire);
    if (st == kFibDone) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: cannot arm a done fiber's patch: " +
                                 std::to_string(fid));
    }
    // Patch basis: the fiber's RSP at its LAST park — the entry trampoline's
    // frame for a never-started fiber, re-based at every yield by
    // run_on_current (M6-b review M10). Always inside this fiber's own
    // mapping and above every descent deeper than the last park. A fiber
    // that descended below its last park basis and loops there without
    // crossing the patch never traps (the cooperative poll path covers it
    // — documented in docs/infrastructure/04 6.3).
    uintptr_t sp = f->last_sp.load(std::memory_order_acquire);
    if (sp == 0) {
        sp = f->ctx.uc_mcontext.gregs[REG_RSP];
    }
    // Re-arm hygiene (verification round NEW-3): disarm the fiber's live
    // patch first — repeated suspend_fiber calls must not accumulate
    // patches over one record (disarm_record would sweep them at retire,
    // but the stale pages stay hostile until then).
    const uint64_t stale = f->patch_base.exchange(0, std::memory_order_acq_rel);
    if (stale != 0) {
        (void)GuardPageSuspender::disarm(reinterpret_cast<void*>(stale));
    }
    auto armed = GuardPageSuspender::arm_sp_patch(&f->guard,
                                                  reinterpret_cast<void*>(sp));
    if (armed) {
        f->patch_base.store(align_down(sp, os_page_size()) - os_page_size(),
                            std::memory_order_release);
    }
    return armed;
}

// CEP:WHAT: Observability: the fiber's scheduler state (Rule 120).
// CEP:WHY: Tests drive suspension/resume through it deterministically.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Unknown ids -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One atomic load.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<uint32_t> WorkStealingPool::fiber_state(uint64_t fid) const {
    const Impl* impl = impl_;
    if (impl == nullptr) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "threading: pool allocation failed");
    }
    const Fiber* f = impl->fiber(fid);
    if (f == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: unknown fiber id: " +
                                 std::to_string(fid));
    }
    return f->state.load(std::memory_order_acquire);
}

// CEP:WHAT: Runs a fiber on the CALLING thread until yield/done/trap.
// CEP:WHY: Saves/restores the caller's scheduler slots; the fiber may run anywhere.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Unknown ids / resume failures -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One run_on_current.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<WorkStealingPool::RunOutcome> WorkStealingPool::resume_fiber(
    uint64_t fid) {
    Impl* impl = impl_;
    if (impl == nullptr) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "threading: pool allocation failed");
    }
    Fiber* f = impl->fiber(fid);
    if (f == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: unknown fiber id: " +
                                 std::to_string(fid));
    }
    // Remove a QUEUED fiber from the global ready queue before running it
    // here (M6-b review B2c): in driver-only mode this is the only runner,
    // so the entry would otherwise linger and double-schedule the fiber.
    // Worker deques are left alone — the CAS claim in run_on_current turns
    // a stale entry into a counted reject, never a second activation.
    {
        std::lock_guard<std::mutex> lk(impl->mu);
        for (size_t i = 0; i < impl->global_ready.size(); ++i) {
            if (impl->global_ready[i] == fid) {
                impl->global_ready.erase(impl->global_ready.begin() +
                                         static_cast<long>(i));
                break;
            }
        }
    }
    // Save/restore the calling thread's scheduler slots; the fiber runs on
    // THIS thread until its next yield/done/trap.
    WorkStealingPool* saved_pool = t_pool;
    Worker* saved_worker = t_worker;
    ucontext_t* saved_loop = t_loop;
    Fiber* saved_fiber = t_fiber;
    ucontext_t comeback{};
    t_pool = this;
    t_worker = nullptr;
    t_loop = &comeback;
    t_fiber = nullptr;
    auto r = impl->run_on_current(*f);
    t_pool = saved_pool;
    t_worker = saved_worker;
    t_loop = saved_loop;
    t_fiber = saved_fiber;
    return r;
}

// CEP:WHAT: Waits for a fiber to finish and propagates its entry result.
// CEP:WHY: Fibers join by yielding; OS threads park on the pool condvar.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Unknown ids / self-join -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. Bounded waits until Done.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> WorkStealingPool::join(uint64_t fid) {
    Impl* impl = impl_;
    if (impl == nullptr) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "threading: pool allocation failed");
    }
    Fiber* f = impl->fiber(fid);
    if (f == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: unknown fiber id: " +
                                 std::to_string(fid));
    }
    if (t_fiber == f) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: a fiber cannot join itself");
    }
    while (f->state.load(std::memory_order_acquire) != kFibDone) {
        if (t_fiber != nullptr) {
            (void)yield_current();
        } else {
            std::unique_lock<std::mutex> lk(impl->mu);
            impl->cv.wait_for(lk, std::chrono::milliseconds(1));
        }
    }
    if (f->result) return support::ok();
    return support::Result<void>(std::unexpected(f->result.error()));
}

// CEP:WHAT: Wires the safepoint integration (handshake + polling page) into the pool.
// CEP:WHY: adopt_interpreter requires a prior bind.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Null arguments -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. Two pointer stores.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> WorkStealingPool::bind_suspension(
    HandshakeManager* handshake, SafepointPollingPage* page) {
    if (handshake == nullptr || page == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: bind_suspension needs the handshake "
                             "manager and the polling page");
    }
    Impl* impl = impl_;
    if (impl == nullptr) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "threading: pool allocation failed");
    }
    impl->hs = handshake;
    impl->page = page;
    // Register fibers that already exist (M6-b review M6): their polls
    // acknowledge a freeze for real. A full table stops the registrations
    // — unregistered fibers keep hs_id 0 and the freeze does not wait on
    // them (documented degrade). Under impl->mu (M6-c review R2-1): the
    // ownership vector's reallocation discipline is "every vector access
    // under mu", and bind_suspension can run concurrently with spawn.
    // Lock order impl->mu -> hs matches spawn's established order.
    std::lock_guard<std::mutex> lk(impl->mu);
    for (auto& f : impl->fibers) {
        if (f->hs_id != 0) continue;
        auto id = handshake->register_thread();
        if (!id) break;
        f->hs_id = *id;
    }
    return support::ok();
}

// CEP:WHAT: Installs the pool's poll hook into an interpreter (not owned).
// CEP:WHY: The hook is the T0 slow path; unadopted interpreters keep their own.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Missing bind / null interpreter -> named rejects.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One hook install + one vector push.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> WorkStealingPool::adopt_interpreter(
    vm::Interpreter* interpreter) {
    Impl* impl = impl_;
    if (impl == nullptr) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "threading: pool allocation failed");
    }
    if (impl->hs == nullptr) {
        return support::fail(support::ErrorCode::InvalidState,
                             "threading: bind_suspension before "
                             "adopt_interpreter");
    }
    if (interpreter == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "threading: adopt_interpreter needs an "
                             "interpreter");
    }
    interpreter->set_safepoint_hook(&WorkStealingPool::poll_hook, this);
    impl->adopted.push_back(interpreter);
    return support::ok();
}

// CEP:WHAT: Stops spawning, drains every queue, joins the workers.
// CEP:WHY: Idempotent; suspended fibers reject the drain by name.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: ShutdownWithSuspendedFibers -> named reject (reported once).
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. N pthread_joins.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
support::Result<void> WorkStealingPool::shutdown() {
    Impl* impl = impl_;
    if (impl == nullptr) return support::ok();
    {
        std::lock_guard<std::mutex> lk(impl->mu);
        impl->stop = true;
        impl->cv.notify_all();
    }
    for (auto& w : impl->workers) {
        if (w->started) {
            pthread_join(w->th, nullptr);
            w->started = false;
        }
    }
    std::lock_guard<std::mutex> lk(impl->mu);
    impl->shutdown_done = true;
    for (const auto& f : impl->fibers) {
        if (f->map != nullptr &&
            f->state.load(std::memory_order_acquire) == kFibSuspended) {
            return support::fail(
                support::ErrorCode::InvalidState,
                "threading: shutdown with suspended fibers (resume them "
                "first; ShutdownWithSuspendedFibers)");
        }
    }
    return support::ok();
}

// CEP:WHAT: Observability: did shutdown drain to completion?
// CEP:WHY: Destructors use it to avoid a double join.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a flag read.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One flag read.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
bool WorkStealingPool::shutdown_complete() const noexcept {
    return impl_ != nullptr && impl_->shutdown_done;
}

// CEP:WHAT: Observability counters (spawned/completed/stolen/yields/freeze_polls).
// CEP:WHY: Rule 120: scheduler health is spellable, never guessed.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — atomic loads.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. Five atomic loads.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
WorkStealingPool::Stats WorkStealingPool::stats() const noexcept {
    Stats s;
    if (impl_ == nullptr) return s;
    s.spawned = impl_->s_spawned.load(std::memory_order_relaxed);
    s.completed = impl_->s_completed.load(std::memory_order_relaxed);
    s.stolen = impl_->s_stolen.load(std::memory_order_relaxed);
    s.yields = impl_->s_yields.load(std::memory_order_relaxed);
    s.freeze_polls = impl_->s_freeze_polls.load(std::memory_order_relaxed);
    s.run_rejected = impl_->s_run_rejected.load(std::memory_order_relaxed);
    return s;
}

// CEP:WHAT: Observability: the live worker count (0 = driver-only or degraded).
// CEP:WHY: Spawn placement and tests read it.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a field read.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One field read.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
uint32_t WorkStealingPool::os_thread_count() const noexcept {
    return impl_ != nullptr ? impl_->n_workers : 0;
}

// CEP:WHAT: Observability: the live fiber count (bounded by kMaxFibers).
// CEP:WHY: Tests assert the M:N fan-out through it.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a size read.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One size read.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
uint64_t WorkStealingPool::fiber_count() const noexcept {
    // s_spawned, NOT fibers.size() (M6-c review R2-1): the counter is a
    // relaxed atomic readable from any thread; the vector's size() is
    // mu-protected (spawn push_back may reallocate). The table is
    // append-only (records are kept for join), so the counter and size()
    // are equal at every point size() is legally readable.
    return impl_ != nullptr ? impl_->s_spawned.load(std::memory_order_relaxed)
                            : 0;
}

// CEP:WHAT: The safepoint slow path: acknowledge, wait out freezes, rotate quanta.
// CEP:WHY: One relaxed load on the fast path; the slow path blocks cooperatively.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the word state drives everything.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @hot. One relaxed load + rare bounded spin.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
void WorkStealingPool::poll_hook(void* user) {
    auto* pool = static_cast<WorkStealingPool*>(user);
    Impl& impl = *pool->impl_;
    Fiber* f = t_fiber;
    std::atomic<uint32_t>* word = SafepointPollingPage::poll_word();
    if (word == nullptr) {
        // No polling page reserved: quantum rotation only.
        if (f != nullptr && ++t_poll_count >= kYieldQuantum) {
            t_poll_count = 0;
            (void)pool->yield_current();
        }
        return;
    }
    if (word->load(std::memory_order_relaxed) != 0) {
        impl.s_freeze_polls.fetch_add(1, std::memory_order_relaxed);
        if (impl.hs != nullptr && f != nullptr && f->hs_id != 0) {
            (void)impl.hs->acknowledge(f->hs_id);
        }
        uint32_t spins = 0;
        while (word->load(std::memory_order_relaxed) != 0) {
            if (++spins >= kYieldQuantum) {
                spins = 0;
                (void)pool->yield_current();  // requeues; the fiber returns
            } else {
                sched_yield();
            }
        }
        return;
    }
    // Disarmed: M:N rotation every quantum executed polls.
    if (f != nullptr && ++t_poll_count >= kYieldQuantum) {
        t_poll_count = 0;
        (void)pool->yield_current();
    }
}

// CEP:WHAT: Shuts down, joins workers, and reclaims every fiber mapping.
// CEP:WHY: Suspended fibers are reported by shutdown; mappings never leak.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — best-effort cleanup.
// CEP:ASSUMES: Callers hold the documented lifetimes; the module-level
//           CEP:ASSUMES covers platform and signal-safety contracts.
// CEP:COST: @cold. One shutdown + N munmaps.
// CEP:EVIDENCE: tests/test_threading.cpp (the DoD list, docs/
//           infrastructure/04 section 10).
WorkStealingPool::~WorkStealingPool() {
    if (impl_ == nullptr) return;
    if (!impl_->shutdown_done) (void)shutdown();
    for (auto& f : impl_->fibers) {
        if (f->map != nullptr) {
            // shutdown() rejects suspended fibers by name; the destructor
            // has no result channel — fail the fiber explicitly so its
            // record reports the truth, and release its handshake slot
            // before reclaiming the mapping (M6-b review m5: no silent
            // bypass of ShutdownWithSuspendedFibers on the dtor path).
            if (f->state.load(std::memory_order_acquire) == kFibSuspended &&
                !f->result) {
                f->result = support::fail(
                    support::ErrorCode::InvalidState,
                    "threading: fiber suspended at pool destruction: " +
                        std::to_string(f->id));
            }
            if (f->hs_id != 0 && impl_->hs != nullptr) {
                (void)impl_->hs->unregister_thread(f->hs_id);
                f->hs_id = 0;
            }
            (void)GuardPageSuspender::disarm(f->map);
            (void)GuardPageSuspender::disarm_record(&f->guard);
            (void)munmap(f->map, f->map_bytes);
            f->map = nullptr;
        }
    }
    delete impl_;
    impl_ = nullptr;
}

}  // namespace vortex::infra

