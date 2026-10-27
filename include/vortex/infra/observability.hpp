// Infra 6 — Observability, Debugging & Tooling
// (docs/infrastructure/06-observability.md).
//
// M6-d ships the observability floor: the perf-jitdump v1 writer + the
// perf-<pid>.map sidecar behind the JitSession singleton, the safepoint
// TTSP log fed by the handshake manager, and the TLAB-refill allocation
// sampler. TierEventSink is the M0-real stream consumed by `vx stats`.
// The GDB JIT interface, managed sanitizers, and deterministic replay
// remain contract stubs (batch e).
// CEP:FILE: include/vortex/infra/observability.hpp
// CEP:WHAT: The observability floor — jitdump/perf-map code telemetry,
//           safepoint TTSP rings, allocation sampling — plus the M0
//           TierEventSink and the batch-e contract stubs.
// CEP:WHY: docs/infrastructure/06-observability.md (the R40 floor) — OS
//          tools must attribute JIT frames, and every performance item
//          needs its measurement source; Rule 120 telemetry.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete (JitDumpWriter/PerfMap/JitSession/SafepointLog/
//          AllocSampler/TierEventSink); GdbJitInterface/SanitizerPolicy/
//          ReplayRecorder are contract stubs (batch e).
// CEP:FAILURE: Writer/session API surfaces return named Result failures
//          (uncreatable paths, double-open, use-after-stop). The PASSIVE
//          hooks (JitSession::record_code, SafepointLog::record,
//          AllocSampler::on_refill) never fail their callers — telemetry
//          failure stores/degrades, never rejects correct execution.
// CEP:ASSUMES: ONE jitdump producer per process (perf's requirement —
//          double-open rejects by name). Publish-path records run on the
//          driver thread; sampler feeds come from the single mutator
//          (Rule 83); the safepoint log is called under the handshake
//          manager's own serialization.
// CEP:COST: @cold everywhere except AllocSampler::on_refill (@warm —
//          interval-gated ring push) and the publish-path one-branch
//          active() check.
// CEP:EVIDENCE: tests/test_observability.cpp — the 17-item DoD list in
//          docs/infrastructure/06-observability.md.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "vortex/support/result.hpp"

namespace vortex::infra {

using support::Result;

/// CLOCK_MONOTONIC nanoseconds — the one time source for every telemetry
/// timestamp this module writes (jitdump, sampler, safepoint events).
// CEP:WHAT: The module's shared monotonic clock read.
// CEP:WHY: The jitdump format mandates CLOCK_MONOTONIC ns; one source keeps
//          every ring comparable.
// CEP:STATUS: complete
// CEP:FAILURE: none — returns the raw read (a failed clock read yields 0,
//          which the DoD timestamp tests would surface immediately).
// CEP:ASSUMES: Linux CLOCK_MONOTONIC (the M6-b platform contract).
// CEP:COST: @cold/@warm. One vDSO clock_gettime.
// CEP:EVIDENCE: asserted transitively by every timestamp DoD test.
uint64_t mono_clock_ns() noexcept;

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

// ---- perf-jitdump v1 ----------------------------------------------------------

// CEP:WHAT: perf-jitdump v1 file writer (docs/infrastructure/06 section 2):
//           40-byte header, unaligned packed records, CODE_LOAD/CLOSE,
//           CLOCK_MONOTONIC timestamps, the PROT_EXEC self-mmap perf
//           requires for discovery.
// CEP:WHY: The perf/VTune attribution path for JIT frames (the R40 floor's
//          headline mechanism).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: open on an uncreatable path, record/close on a closed writer,
//          open while open — named Result failures; byte-level write errors
//          surface as named I/O failures (never silent truncation).
// CEP:ASSUMES: One writer per process (JitSession enforces); the caller
//          passes code ranges that are live for the process lifetime of the
//          record (publish-path discipline — the bytes are final).
// CEP:COST: @cold. One write() syscall per record; one mmap/munmap per open/
//          close.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 1-7 (parse-back, timestamps,
//          self-mmap probe, named rejects).
class JitDumpWriter {
public:
    /// One source-line debug entry (format RESERVED by the v1 spec; M6-d
    /// writes nr_debug_entries = 0 — real entries are batch e's DWARF work).
    struct DebugEntry {
        uint64_t addr = 0;
        uint32_t line = 0;
        uint32_t discriminant = 0;
        std::string name;
    };

    JitDumpWriter() = default;
    ~JitDumpWriter();
    JitDumpWriter(JitDumpWriter&& other) noexcept;
    JitDumpWriter& operator=(JitDumpWriter&& other) noexcept;
    JitDumpWriter(const JitDumpWriter&) = delete;
    JitDumpWriter& operator=(const JitDumpWriter&) = delete;

    /// Creates/truncates the file, writes the 40-byte header, installs the
    /// PROT_READ|PROT_EXEC self-mmap over the header extent.
    Result<void> open(const std::string& path);
    /// JIT_CODE_LOAD: pid/tid, vma, code_addr, code_size, code_index, the
    /// NUL-terminated name, nr_debug_entries (+ reserved-format entries).
    Result<void> record_code_load(uint64_t vma, uint64_t code_addr,
                                  uint64_t code_size, uint64_t code_index,
                                  const std::string& name,
                                  std::span<const DebugEntry> debug = {});
    /// JIT_CODE_CLOSE framing record (also written by close()).
    Result<void> record_close();
    Result<void> close();
    bool is_open() const noexcept { return open_; }
    /// Bytes written so far (header + records) — the DoD parse-back's bound.
    uint64_t file_size() const noexcept { return bytes_written_; }

private:
    Result<void> write_raw(const void* data, size_t bytes);
    Result<void> write_u32(uint32_t value);
    Result<void> write_u64(uint64_t value);
    Result<void> write_name(const std::string& name);
    Result<void> require_open(const char* what) const;

    int fd_ = -1;
    void* mmap_base_ = nullptr;
    size_t mmap_bytes_ = 0;
    uint64_t bytes_written_ = 0;
    bool open_ = false;
};

// CEP:WHAT: The perf-<pid>.map text sidecar: "<hex_start> <hex_size> <name>"
//           per code load, appended in record order, names sanitized
//           (space/newline -> '_') because the format is whitespace-
//           delimited (HotSpot's sanitizer is the precedent).
// CEP:WHY: perf's fallback symbolizer when jitdump is unavailable — the
//          floor ships BOTH paths.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: open on an uncreatable path, record/close on a closed map —
//          named Result failures.
// CEP:ASSUMES: One map writer per process (JitSession enforces); files
//          persist after close (perf reads them after process exit — tests
//          use explicit paths and clean up).
// CEP:COST: @cold. One write() per record.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 8 (format + sanitization).
class PerfMap {
public:
    PerfMap() = default;
    ~PerfMap();
    PerfMap(PerfMap&& other) noexcept;
    PerfMap& operator=(PerfMap&& other) noexcept;
    PerfMap(const PerfMap&) = delete;
    PerfMap& operator=(const PerfMap&) = delete;

    Result<void> open(const std::string& path);
    Result<void> record(uint64_t start, uint64_t size, const std::string& name);
    Result<void> close();
    bool is_open() const noexcept { return open_; }

private:
    int fd_ = -1;
    bool open_ = false;
};

// CEP:WHAT: The ONE jitdump producer per process: JitDumpWriter + PerfMap
//           behind a start/stop facade; the code publish path's only
//           touchpoint is record_code() — best-effort, never fails the
//           publish (a stored last_error() is the observable, Rule 120).
// CEP:WHY: Telemetry must never reject correctly-compiled code (full-disk
//          class of failures); and publish sites must stay one branch.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: start/start_default — named rejects (double-open, uncreatable
//          paths; a partial start rolls the already-opened side back);
//          stop on an inactive session — named reject. record_code never
//          fails (stores last_error).
// CEP:ASSUMES: Called on the driver thread (publish-path discipline);
//          singleton state is process-lifetime.
// CEP:COST: @cold. record_code: one active() branch when idle.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 5/6/9/10.
class JitSession {
public:
    /// /tmp/perf-<pid>.jitdump and /tmp/perf-<pid>.map (the live convention).
    static std::string default_jitdump_path();
    static std::string default_perf_map_path();

    /// Opens both sides; a failure on the second rolls the first back.
    Result<void> start(const std::string& jitdump_path,
                       const std::string& perf_map_path);
    Result<void> start_default() {
        return start(default_jitdump_path(), default_perf_map_path());
    }
    /// CODE_CLOSE + close on both writers; keeps the files (perf reads them
    /// after process exit).
    Result<void> stop();
    /// The publish hook: one synthetic symbol per code range,
    /// "vx_<tier>_m<method_id>" (real names are the batch-e metadata
    /// upgrade). Never fails the caller.
    void record_code(const void* code_base, size_t code_bytes,
                     uint32_t method_id, const char* tier) noexcept;
    bool active() const noexcept { return active_; }
    /// The stored best-effort failure ("" when none).
    const std::string& last_error() const noexcept { return last_error_; }
    uint64_t code_loads() const noexcept { return code_loads_; }

private:
    JitDumpWriter jitdump_;
    PerfMap perf_map_;
    uint64_t code_index_ = 0;
    uint64_t code_loads_ = 0;
    bool active_ = false;
    std::string last_error_;
};

/// The process singleton (always valid; state changes through start/stop).
// CEP:WHAT: The singleton accessor.
// CEP:WHY: One producer per process; publish sites take one branch.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Function-local static init (thread-safe, C++11 magic
//          statics).
// CEP:COST: @cold. One guard load after first init.
// CEP:EVIDENCE: consumed by the J1/J2 publish sites; DoD 9/10.
JitSession& jit_session() noexcept;

// ---- safepoint TTSP log --------------------------------------------------------

// CEP:WHAT: One suspension the handshake manager drove: a stop-the-world
//           freeze (ttsp = arm -> last acknowledge; body = last ack ->
//           thaw) or a single-thread handshake (ttsp = request -> ack).
// CEP:WHY: docs/infrastructure/06 section 5 — the p99 attribution source
//          (cause, TTSP, pause body) the measurement plan requires.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: none (a plain record).
// CEP:ASSUMES: cause is the SuspensionKind value (uint8 — no header cycle
//          with threading.hpp).
// CEP:COST: @cold. Plain struct.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 11-13.
struct SafepointEvent {
    enum class Kind : uint8_t { Freeze = 0, Handshake };
    Kind kind = Kind::Freeze;
    uint8_t cause = 0;
    uint64_t ttsp_ns = 0;
    uint64_t body_ns = 0;
    uint32_t threads = 0;
    bool timed_out = false;
    uint64_t generation = 0;
};

// CEP:WHAT: Bounded TTSP ring (512 events; overflow CLEARS — the Rule-28
//           discipline the GC pause ring uses) + nearest-rank percentile
//           helpers over the recorded ttsp values.
// CEP:WHY: Rule 28 bounded telemetry; the percentile shape mirrors GcStats
//          so the two p99 numbers are computed identically.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — record() never fails; overflow is the documented
//          clear. total_events() keeps the true count across clears.
// CEP:ASSUMES: Called under the handshake manager's serialization (its
//          critical sections) or from the single driver thread.
// CEP:COST: @cold. Push + amortized percentile sort on demand.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 11-14.
class SafepointLog {
public:
    static constexpr size_t kMaxEvents = 512;

    void record(SafepointEvent event);
    std::span<const SafepointEvent> events() const noexcept { return events_; }
    /// Every event ever recorded (survives ring clears) — Rule 120 honesty.
    uint64_t total_events() const noexcept { return total_events_; }
    uint64_t ttsp_percentile_ns(unsigned pct) const;
    uint64_t ttsp_p99_ns() const { return ttsp_percentile_ns(99); }
    uint64_t ttsp_max_ns() const;

private:
    std::vector<SafepointEvent> events_;
    uint64_t total_events_ = 0;
};

// ---- allocation sampler --------------------------------------------------------

// CEP:WHAT: One TLAB-refill sample: timestamp, the refill's bytes, the
//           cumulative allocated bytes, and the RESERVED site id (0 =
//           unknown until the emitted-poll batch's unbiased stack walk —
//           the documented JEP-331 attribution deviation).
// CEP:WHY: docs/infrastructure/06 section 6.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: none (a plain record).
// CEP:ASSUMES: Fed by the single mutator's refill path.
// CEP:COST: @cold. Plain struct.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 16.
struct AllocSample {
    uint64_t timestamp_ns = 0;
    uint64_t refill_bytes = 0;
    uint64_t cumulative_bytes = 0;
    uint32_t site_id = 0;  // reserved 0 = unknown (attribution rides R31)
};

// CEP:WHAT: JEP-331-shape TLAB-refill sampling: refill bytes accumulate;
//           each interval crossing records one sample into a bounded ring
//           (4096, Rule-28 clear-on-overflow). Interval-driven — the hot
//           bump path never calls this; the WARM refill path pays one
//           branch + add when idle.
// CEP:WHY: Allocation profiling at near-zero steady-state cost (R40's
//          fourth mechanism).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: set_interval_bytes rejects 0 by name. on_refill never fails.
// CEP:ASSUMES: The single mutator calls on_refill (Rule 83); counters are
//          monotonic and never cleared (Rule 120).
// CEP:COST: @warm. on_refill: one add + compare steady-state, one ring push
//          per interval crossing.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 16 (interval exactness,
//          ring bound, fast-path independence).
class AllocSampler {
public:
    static constexpr uint64_t kDefaultIntervalBytes = 512 * 1024;
    static constexpr size_t kMaxSamples = 4096;

    AllocSampler() = default;

    Result<void> set_interval_bytes(uint64_t interval);
    uint64_t interval_bytes() const noexcept { return interval_bytes_; }
    /// The Heap::refill_tlab hook (AFTER a successful rewindow).
    void on_refill(uint64_t refill_bytes) noexcept;
    std::span<const AllocSample> samples() const noexcept { return samples_; }
    uint64_t refills_observed() const noexcept { return refills_observed_; }
    /// Every sample ever taken (survives ring clears) — Rule 120 honesty.
    uint64_t total_samples() const noexcept { return total_samples_; }
    uint64_t total_refill_bytes() const noexcept { return total_refill_bytes_; }

private:
    uint64_t interval_bytes_ = kDefaultIntervalBytes;
    uint64_t accumulated_bytes_ = 0;
    uint64_t refills_observed_ = 0;
    uint64_t total_samples_ = 0;
    uint64_t total_refill_bytes_ = 0;
    std::vector<AllocSample> samples_;
};

/// The process singleton (matches gc_code_registry's accessor shape).
// CEP:WHAT: The sampler accessor.
// CEP:WHY: Heap::refill_tlab and `vx stats` share one instance.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Function-local static init.
// CEP:COST: @cold/@warm. One guard load after first init.
// CEP:EVIDENCE: consumed by Heap::refill_tlab; DoD 16/17.
AllocSampler& alloc_sampler() noexcept;

// ---- batch-e contract stubs ----------------------------------------------------

/// GDB JIT Interface: dynamic DWARF registration for native debuggers.
/// Contract stub (batch e).
class GdbJitInterface {
public:
    static Result<void> register_code(const void* code,
                                      size_t size,
                                      const char* symbol);
    static Result<void> unregister_code(const void* code);
};

/// Managed sanitizer switches (ASan-for-UGB / TSan-for-UGB instrumentation
/// applied by J2/J3). Contract stub (batch e).
struct SanitizerPolicy {
    bool guest_asan = false;  // shadow-memory checks for guest buffers
    bool guest_tsan = false;  // instrumentation of guest atomics/fields
};

/// Deterministic replay recorder: thread scheduling, IC updates, OSR
/// transitions -> exact time-travel replay. Contract stub (batch e).
class ReplayRecorder {
public:
    void record_event(uint32_t kind, uint64_t payload);
    size_t event_count() const noexcept { return log_.size(); }

private:
    std::vector<std::pair<uint32_t, uint64_t>> log_;
};

}  // namespace vortex::infra
