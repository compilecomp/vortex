// Infra 6 — Observability, Debugging & Tooling
// (docs/infrastructure/06-observability.md).
//
// M6-d shipped the observability floor (jitdump/perf-map, TTSP log,
// sampler); batch e completes the item: the GDB JIT interface (canonical
// descriptor + breakpoint + deterministic ELF64 symfiles), the managed
// sanitizers (guest-heap shadow + verify_span + validated policy), and
// deterministic replay (the decision ring + first-divergence checker).
// TierEventSink is the M0-real stream consumed by `vx stats`.
// CEP:FILE: include/vortex/infra/observability.hpp
// CEP:WHAT: The observability floor — jitdump/perf-map code telemetry,
//           safepoint TTSP rings, allocation sampling — plus the M0
//           TierEventSink and the batch-e contract stubs.
// CEP:WHY: docs/infrastructure/06-observability.md (the R40 floor) — OS
//          tools must attribute JIT frames, and every performance item
//          needs its measurement source; Rule 120 telemetry.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete (JitDumpWriter/PerfMap/JitSession/SafepointLog/
//          AllocSampler/TierEventSink + the batch-e GdbJitInterface/
//          SanitizerPolicy+GuestShadow/ReplayRecorder).
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
// CEP:EVIDENCE: tests/test_observability.cpp — the 33-item DoD list in
//          docs/infrastructure/06-observability.md.
#pragma once

#include <cstdint>
#include <memory>
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
//          surface as named I/O failures (never silent truncation). The
//          debug-entry form serializes nr_debug_entries + the 16-byte
//          entries (batch e; the M6-d refusal is retired).
// CEP:ASSUMES: One writer per process (JitSession enforces); the caller
//          passes code ranges that are live for the process lifetime of the
//          record (publish-path discipline — the bytes are final).
// CEP:COST: @cold. One write() syscall per record; one mmap/munmap per open/
//          close.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 1-7, 32 (parse-back,
//          timestamps, self-mmap probe, named rejects, line entries).
class JitDumpWriter {
public:
    /// One source-line debug entry (the v1 spec's reserved format: u64
    /// addr, u32 line, u32 column — serialized by record_code_load; the
    /// name field is unused by the line-entry form).
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
    /// "vx_<tier>_m<method_id>" (the empty-name fallback shape). Never
    /// fails the caller.
    void record_code(const void* code_base, size_t code_bytes,
                     uint32_t method_id, const char* tier) noexcept;
    /// The named+line publish hook (batch e metadata interchange): the
    /// real method symbol and its source-line entries. An empty name
    /// falls back to the synthetic form. Never fails the caller.
    void record_code(const void* code_base, size_t code_bytes,
                     const std::string& name,
                     std::span<const JitDumpWriter::DebugEntry> debug) noexcept;
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

// ---- batch e: GDB JIT interface -------------------------------------------------

/// The canonical gdb client entry (gdb/jit.h ABI — the exact field order
/// the debugger reads: next/prev links first, then the symfile pointer/size).
// CEP:WHAT: One registered code blob's descriptor-list entry.
// CEP:WHY: docs/infrastructure/06 GDB JIT interface section — the ABI gdb
//          discovers at attach time; layout IS the contract (gdb/jit.h's
//          jit_code_entry: next_entry, prev_entry, symfile_addr,
//          symfile_size).
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: none (a plain struct).
// CEP:ASSUMES: LP64 (u64 symfile_addr); mutated only under the interface's
//          mutex.
// CEP:COST: @cold. Plain struct.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 19-22.
struct GdbJitCodeEntry {
    GdbJitCodeEntry* next_entry = nullptr;  // the head-side link
    GdbJitCodeEntry* prev_entry = nullptr;  // the tail-side link
    uint64_t symfile_addr = 0;
    uint64_t symfile_size = 0;
};

/// The canonical gdb descriptor + its debugger-visible variable (gdb/jit.h
/// ABI). Raw fields, static zero-init except `version = 1` (set by the
/// definition); C linkage so the symbol name is unmangled.
// CEP:WHAT: The descriptor gdb reads (version + action + entry chain).
// CEP:WHY: The interface's second half — the breakpoint function signals
//          "descriptor changed", the descriptor carries the state.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Defined exactly once (src/infra/observability.cpp).
// CEP:COST: @cold. Static storage.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 18.
extern "C" {
struct GdbJitDescriptor {
    uint32_t version;         // 1 (the definition initializes it)
    uint32_t action_flag;     // kJitNoAction / Register / Unregister
    GdbJitCodeEntry* relevant_entry;  // legacy, zeroed
    GdbJitCodeEntry* first_entry;     // the chain head
};
extern GdbJitDescriptor __jit_debug_descriptor;
}

// CEP:WHAT: The debugger's breakpoint target: gdb sets its breakpoint here
//           and reads __jit_debug_descriptor when it hits.
// CEP:WHY: The interface's signaling half (gdb/jit.h ABI).
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: none (an empty function — the breakpoint IS the effect).
// CEP:ASSUMES: defined once, never inlined, never eliminated (used+noinline).
// CEP:COST: @cold. One ret.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 18 (dlsym resolves it).
extern "C" void __jit_debug_register_code(void) noexcept;

// CEP:WHAT: The GDB JIT interface client: registers/unregisters code blobs
//           with deterministic in-memory ELF64 symfiles through the
//           canonical descriptor + breakpoint pair.
// CEP:WHY: docs/infrastructure/06 GDB JIT interface section — native
//          debuggers must name JIT frames (the debugger leg of the
//          observability item).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: register — null/zero-size, duplicate live base, OOM;
//          unregister — unknown base; all named Result rejects. The mutex
//          serializes descriptor/chain mutation.
// CEP:ASSUMES: @cold only (publish path + teardown; never signal context);
//          registration snapshots the bytes (the live W^X mapping is never
//          referenced afterward).
// CEP:COST: @cold. One memcpy + small vector build per registration.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 18-22.
class GdbJitInterface {
public:
    // Action flags (gdb/jit.h ABI).
    static constexpr uint32_t kJitNoAction = 0;
    static constexpr uint32_t kJitRegisterFn = 1;
    static constexpr uint32_t kJitUnregisterFn = 2;

    static Result<void> register_code(const void* code, size_t size,
                                      const char* symbol);
    static Result<void> unregister_code(const void* code);
    /// Registers and returns an owning token whose destruction
    /// unregisters; a named failure returns null (the publish path never
    /// fails for telemetry).
    static std::shared_ptr<void> register_code_owned(const void* code,
                                                     size_t size,
                                                     const char* symbol);

    // Observability for tests/tooling (mutex-guarded reads).
    static size_t live_entries();
    static bool is_registered(const void* code);
    /// The symfile bytes of a live registration (tests parse the ELF back).
    static std::span<const uint8_t> symfile_of(const void* code);
};

// ---- batch e: managed sanitizers -------------------------------------------------

// CEP:WHAT: The managed-sanitizer process config: guest_asan (the guest-
//           heap shadow) is supported; guest_tsan is RESERVED — validation
//           refuses it by name until the atomics capability pack ships.
// CEP:WHY: docs/infrastructure/06 managed sanitizers section — a reserved
//          bit must never silently no-op (Rule 120).
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: make() refuses guest_tsan by name (named Result reject).
// CEP:ASSUMES: Installed once via attach_sanitizer_policy (the single
//          mutator's setup, before guest code runs).
// CEP:COST: @cold. Two bools.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 23.
struct SanitizerPolicy {
    bool guest_asan = false;  // shadow-memory checks for guest spans
    bool guest_tsan = false;  // RESERVED (refused by make())

    /// The only sanctioned construction: validation is the gate.
    static Result<SanitizerPolicy> make(bool guest_asan, bool guest_tsan);
};

/// Validates + installs the process policy (once; a second attach with a
/// DIFFERENT policy is a named reject — flipping sanitizer state under live
/// code is an untracked assumption change).
// CEP:WHAT: The policy attach point.
// CEP:WHY: One validated process-wide config; consumers read one bool.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: reserved-bit refusal (via make); re-attach mismatch reject.
// CEP:ASSUMES: Called before guest code runs (driver/test setup).
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 23/27.
Result<void> attach_sanitizer_policy(const SanitizerPolicy& policy);

/// The installed policy (default: everything off).
// CEP:WHAT: The policy accessor.
// CEP:WHY: The consumers' one-branch gate reads this.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Function-local static init.
// CEP:COST: @warm — one guard load per read (the disabled-path cost
//          contract in the spec).
// CEP:EVIDENCE: the T0/FFI consumer sites.
SanitizerPolicy& sanitizer_policy() noexcept;

// CEP:WHAT: The guest-heap shadow: 1 byte per 8 heap bytes over the heap
//           span (constructed exactly like the card table); marks
//           evacuated-from copies and released-region spans.
// CEP:WHY: docs/infrastructure/06 managed sanitizers section — the GC-
//          corruption detection classes (stale references into evacuated
//          copies / released regions) at the boundaries guest + foreign
//          code actually touch.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — poisoning is a store; detection is verify_span's
//          named failure.
// CEP:ASSUMES: One heap's span per shadow; producers are @cold (GC phases,
//          region release); young bump space is NEVER poisoned.
// CEP:COST: @cold producers (span fill); the check is verify_span.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 24/25/27.
class GuestShadow {
public:
    static constexpr uint8_t kShadowShift = 3;  // 1 shadow byte per 8
    static constexpr uint8_t kAddressable = 0;
    static constexpr uint8_t kEvacuated = 0xE1;
    static constexpr uint8_t kReleased = 0xE2;

    GuestShadow(const void* heap_base, size_t heap_bytes);

    void poison(uint64_t addr, size_t bytes, uint8_t value) noexcept;
    void unpoison(uint64_t addr, size_t bytes) noexcept;
    /// The shadow byte governing `addr` (0 outside the heap) — the
    /// SanitizerTrap replay event's kind value.
    uint8_t poison_kind(const void* addr) const noexcept;
    /// First+last granule check (the standard partial-granule shape).
    bool span_ok(const void* addr, size_t bytes) const noexcept;
    const void* heap_base() const noexcept { return base_ptr_; }
    size_t heap_bytes() const noexcept { return heap_bytes_; }

private:
    const void* base_ptr_ = nullptr;
    uint64_t base_ = 0;
    size_t heap_bytes_ = 0;
    std::vector<uint8_t> shadow_;
};

/// The attached shadow (the Heap installs its instance; raw, no ownership —
/// the heap outlives every guest execution). Null until a heap attaches.
// CEP:WHAT: The shadow accessor + attach point.
// CEP:WHY: verify_span routes through the CURRENT heap's shadow; the raw
//          pointer matches gc_code_registry's accessor discipline.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Single heap live per engine instance (Rule 83 world); the
//          Heap detaches at destruction.
// CEP:COST: @cold attach; the accessor is one guard load.
// CEP:EVIDENCE: the Heap poison producers + verify_span.
GuestShadow* guest_shadow() noexcept;
void attach_guest_shadow(GuestShadow* shadow) noexcept;

/// The ONE check everything routes through (T0 ops, the FFI boundary,
/// tests). Policy-disabled or no shadow: always ok (the cost contract).
// CEP:WHAT: The sanitizer span check.
// CEP:WHY: One named failure shape for every consumer (kind + address in
//          the diagnostic).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: named GuestSanitizer failure carrying the violating kind
//          + address; NEVER aborts the process.
// CEP:ASSUMES: @warm when enabled (one shadow byte load per span); one
//          relaxed bool load when disabled.
// CEP:COST: disabled — one bool load; enabled — first+last granule load.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 24-27.
Result<void> verify_span(const void* addr, size_t bytes) noexcept;

// ---- batch e: deterministic replay -------------------------------------------------

/// The recorded decision kinds (the payload packing per kind is normative
/// in docs/infrastructure/06 — deterministic replay section).
enum class ReplayKind : uint32_t {
    TierHandoff = 0,  // a = method_id, b = from | (to << 32)
    OsrEntry,         // a = method_id, b = bytecode pc
    IcTransition,     // a = from | (to << 32), b = the entry's klass id
    GcCycleStart,     // a = cause
    Deopt,            // a = method_id, b = cause
    Publish,          // a = method_id, b = tier ordinal
    SanitizerTrap,    // a = address low bits, b = kind value
};

// CEP:WHAT: One recorded engine decision (seq monotonic, kind + payload).
// CEP:WHY: The differential checker compares decision streams; the seq
//          survives ring clears (Rule 28 + Rule 120).
// CEP:STATUS: complete
// CEP:FAILURE: none (a plain record).
// CEP:ASSUMES: Single-producer sites (the driver/mutator threads per site).
// CEP:COST: @cold. Plain struct.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 28-31.
struct ReplayEvent {
    uint64_t seq = 0;
    uint32_t kind = 0;  // ReplayKind value
    uint64_t a = 0;
    uint64_t b = 0;
};

// CEP:WHAT: The decision ring (bounded 65536, Rule-28 clear-on-overflow,
//           monotonic seq) + the global enable flag + the first-divergence
//           comparator.
// CEP:WHY: docs/infrastructure/06 deterministic replay section — the
//          engine-wide determinism property ("same program + same input +
//          same policy -> same decision stream") needs the stream and the
//          differ; exact re-execution scheduling is NOT the scope.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — record under a disabled flag is a no-op (the cost
//          contract); overflow is the documented clear.
// CEP:ASSUMES: Producers check replay_enabled() (the one flag) — off means
//          zero events, zero cost.
// CEP:COST: disabled — one relaxed bool load per producer site; enabled —
//          one ring push per decision.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 28-31.
class ReplayRecorder {
public:
    static constexpr size_t kMaxEvents = 65536;

    /// No-op unless the replay flag is on (the producers' contract).
    void record_event(uint32_t kind, uint64_t a, uint64_t b) noexcept;
    std::span<const ReplayEvent> events() const noexcept { return events_; }
    /// Every event ever recorded (survives ring clears) — Rule 120 honesty.
    uint64_t total_events() const noexcept { return total_events_; }
    void clear() noexcept;

private:
    std::vector<ReplayEvent> events_;
    uint64_t total_events_ = 0;
    uint64_t seq_ = 0;
};

// CEP:WHAT: The replay flag accessors — the ONE switch every producer site
//           reads.
// CEP:WHY: One flag keeps the zero-cost-off contract auditable.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Flipped before the run under test (mid-run flips only add
//          events on one side — the differential harness controls runs).
// CEP:COST: one relaxed bool load per read.
// CEP:EVIDENCE: DoD 31.
bool replay_enabled() noexcept;
void set_replay_enabled(bool enabled) noexcept;

/// The process singleton (the producers' shared ring).
// CEP:WHAT: The recorder accessor.
// CEP:WHY: Tiering/T0/GC/RBPD/publish/sanitizer producers share one stream.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Function-local static init.
// CEP:COST: one guard load after first init.
// CEP:EVIDENCE: the producer sites.
ReplayRecorder& replay_recorder() noexcept;

// CEP:WHAT: The first-divergence report: identical flag, the diverging
//           index, and both sides' payloads there.
// CEP:WHY: The differential harness's output — the first divergence IS the
//          diagnosis.
// CEP:STATUS: complete
// CEP:FAILURE: none (a pure comparison).
// CEP:ASSUMES: O(n) walk over both rings.
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 29.
struct ReplayDivergence {
    bool identical = true;
    size_t first_divergence = 0;
    uint32_t kind = 0;
    uint64_t expected_a = 0;
    uint64_t expected_b = 0;
    uint64_t actual_a = 0;
    uint64_t actual_b = 0;
};

ReplayDivergence compare_streams(const ReplayRecorder& expected,
                                 const ReplayRecorder& actual) noexcept;

}  // namespace vortex::infra
