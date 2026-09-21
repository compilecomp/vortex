// Infra 4 — Advanced Threading & Suspension
// (docs/infrastructure/04-threading-suspension.md).
//
// M:N green thread scheduler, handshake-based asymmetric suspension, and the
// memory-protection safepoint polling page. Contract-stubbed in M0; T0 already
// executes cooperative safepoint polls at backward branches, so the polling
// contract has a consumer today.
#pragma once

#include <cstdint>
#include <vector>

#include "vortex/support/result.hpp"

namespace vortex::infra {

enum class SuspensionKind : uint8_t { Gc, Deopt, Profiling, Debugger };

/// A guest coroutine/fiber context scheduled onto OS worker threads (M:N).
struct GreenThread {
    uint64_t id = 0;
    void* stack_base = nullptr;
    size_t stack_size = 0;
    uint32_t state = 0;  // scheduler-private
};

/// Work-stealing scheduler mapping millions of guest fibers onto a fixed OS
/// thread pool. Contract stub.
class WorkStealingPool {
public:
    explicit WorkStealingPool(uint32_t os_threads);
    support::Result<uint64_t> spawn(GreenThread thread);
    support::Result<void> yield(uint64_t thread_id);
    uint32_t os_thread_count() const noexcept { return os_threads_; }
    uint64_t scheduled_count() const noexcept { return scheduled_; }

private:
    uint32_t os_threads_;
    uint64_t scheduled_ = 0;
    std::vector<GreenThread> threads_;
};

/// Handshake request/acknowledge protocol between the runtime and running
/// threads (asymmetric suspension without OS signals). Contract stub.
class HandshakeManager {
public:
    support::Result<void> request(SuspensionKind kind, uint64_t thread_id);
    support::Result<void> acknowledge(uint64_t thread_id);
    // Contract stub (M2 threading infra): thread_id is part of the public
    // handshake API and is consumed when the real manager lands.
    bool pending([[maybe_unused]] uint64_t thread_id) const noexcept {
        return pending_;
    }

private:
    bool pending_ = false;
};

/// Stack guard-page based suspension: mprotect the thread's stack guard pages;
/// the next guest touch traps into the runtime which yields the thread.
/// Contract stub.
class GuardPageSuspender {
public:
    static support::Result<void> arm(void* stack_base, size_t stack_size);
    static support::Result<void> disarm(void* stack_base, size_t stack_size);
};

/// Safepoint polling page: marked read-only when a GC is needed; the next LOAD
/// from the polling page traps — a zero-overhead safepoint for J3/J4 code.
/// Contract stub (T0 polls cooperatively in M0).
class SafepointPollingPage {
public:
    static support::Result<void*> reserve();
    static support::Result<void> arm();    // flip to PROT_NONE
    static support::Result<void> disarm(); // flip back to readable
};

}  // namespace vortex::infra
