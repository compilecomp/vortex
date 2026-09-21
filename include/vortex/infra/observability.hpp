// Infra 6 — Observability, Debugging & Tooling
// (docs/infrastructure/06-observability.md).
//
// M0 ships the TierEventSink used by `vx stats`; jitdump/GDB/ sanitizers/
// replay are contract-stubbed.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vortex/support/result.hpp"

namespace vortex::infra {

using support::Result;

enum class TierEventKind : uint8_t {
    MethodEnter,
    TierPromote,
    TierDemote,
    OsrRequest,
    GuardFailure,
    RegionRecompile,
    CodeInstall,
    CodeEvict,
};

struct TierEvent {
    TierEventKind kind = TierEventKind::MethodEnter;
    uint32_t method_id = 0;
    uint8_t tier = 0;
    uint64_t counter = 0;
    std::string detail;
};

/// Structured event stream consumed by the `vx stats` sink and tests. Real.
class TierEventSink {
public:
    void record(TierEvent event) { events_.push_back(std::move(event)); }
    std::span<const TierEvent> events() const noexcept { return events_; }
    void clear() noexcept { events_.clear(); }
    uint64_t count(TierEventKind kind) const noexcept {
        uint64_t n = 0;
        for (const auto& e : events_) n += (e.kind == kind);
        return n;
    }

private:
    std::vector<TierEvent> events_;
};

/// perf-jitdump ELF container writer (MMAP_EVENT / CODE_LOAD records mapping
/// machine-code addresses to UGB source lines). Contract stub (docs/roadmap.md M6).
class JitDumpWriter {
public:
    static Result<void> open(const std::string& path);
    static Result<void> record_code_load(
        uint64_t addr, size_t size, const std::string& symbol,
        const std::string& source_line_map);
    static Result<void> close();
};

/// GDB JIT Interface: dynamic DWARF registration for native debuggers.
/// Contract stub.
class GdbJitInterface {
public:
    static Result<void> register_code(const void* code,
                                      size_t size,
                                      const char* symbol);
    static Result<void> unregister_code(const void* code);
};

/// Managed sanitizer switches (ASan-for-UGB / TSan-for-UGB instrumentation
/// applied by J2/J3). Contract stub.
struct SanitizerPolicy {
    bool guest_asan = false;  // shadow-memory checks for guest buffers
    bool guest_tsan = false;  // instrumentation of guest atomics/fields
};

/// Deterministic replay recorder: thread scheduling, IC updates, OSR
/// transitions -> exact time-travel replay. Contract stub.
class ReplayRecorder {
public:
    void record_event(uint32_t kind, uint64_t payload);
    size_t event_count() const noexcept { return log_.size(); }

private:
    std::vector<std::pair<uint32_t, uint64_t>> log_;
};

}  // namespace vortex::infra
