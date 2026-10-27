// CEP:FILE: src/infra/observability.cpp
// CEP:WHAT: The observability floor's implementation: the perf-jitdump v1
//           writer (packed unaligned records, CLOCK_MONOTONIC timestamps,
//           the PROT_EXEC self-mmap), the perf-map sidecar, the JitSession
//           singleton facade with best-effort publish hooks, the safepoint
//           TTSP ring, and the TLAB-refill allocation sampler; the batch-e
//           contract stubs (GDB JIT interface, replay recorder).
// CEP:WHY: docs/infrastructure/06-observability.md (the R40 floor) — OS
//          tools must attribute JIT frames and every performance item needs
//          its measurement source; Rule 120 telemetry with Rule 28 bounds.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Writer/session APIs return named Result failures
//          (uncreatable paths, double-open, use-after-stop); the passive
//          hooks (record_code, SafepointLog::record, on_refill) never fail
//          their callers — degradation + last_error() is the observable.
// CEP:ASSUMES: ONE jitdump producer per process (JitSession enforces);
//          Linux x86-64 (mmap/CLOCK_MONOTONIC/gettid); the single mutator
//          feeds the sampler (Rule 83); publish-path records run on the
//          driver thread with final code bytes.
// CEP:COST: @cold everywhere except AllocSampler::on_refill (@warm,
//          interval-gated) and the publish-path one-branch active() check.
// CEP:EVIDENCE: tests/test_observability.cpp — the DoD 1-16 parse-backs,
//          probe, ring bounds, and named-reject pins.
#include "vortex/infra/observability.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace vortex::infra {

namespace {

// EM_X86_64 (the elf_mach header field — the producer's architecture).
constexpr uint32_t kJitdumpElfMach = 62;
constexpr uint32_t kJitdumpMagic = 0x4A695444u;   // "JiTD" little-endian
constexpr uint32_t kJitdumpVersion = 1;
constexpr uint32_t kJitdumpHeaderSize = 40;
constexpr uint32_t kJitCodeLoadId = 0;
constexpr uint32_t kJitCodeCloseId = 3;

// CEP:WHAT: Appends one little-endian u32 to a record buffer.
// CEP:WHY: The jitdump format is explicitly little-endian and unaligned —
//          explicit byte packing is portable and parse-back-testable.
// CEP:STATUS: complete
// CEP:FAILURE: none (a vector append).
// CEP:ASSUMES: the buffer belongs to one record being assembled.
// CEP:COST: @cold. Four byte stores.
// CEP:EVIDENCE: the DoD parse-back tests read these bytes back.
void push_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

// CEP:WHAT: Appends one little-endian u64 to a record buffer.
// CEP:WHY: Same format rule as push_u32 at twice the width.
// CEP:STATUS: complete
// CEP:FAILURE: none (a vector append).
// CEP:ASSUMES: the buffer belongs to one record being assembled.
// CEP:COST: @cold. Eight byte stores.
// CEP:EVIDENCE: the DoD parse-back tests read these bytes back.
void push_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

// CEP:WHAT: Appends a NUL-terminated name field to a record buffer.
// CEP:WHY: CODE_LOAD names are NUL-terminated per the v1 spec (no length
//          prefix — the record size field bounds the parse).
// CEP:STATUS: complete
// CEP:FAILURE: none (a vector append).
// CEP:ASSUMES: the name contains no embedded NUL (std::string guarantees).
// CEP:COST: @cold. One insert + one store.
// CEP:EVIDENCE: DoD 2 asserts the exact name bytes + the NUL.
void push_name(std::vector<uint8_t>& out, const std::string& name) {
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0);  // NUL-terminated per the format
}

}  // namespace

// CEP:WHAT: The module's shared monotonic clock read (CLOCK_MONOTONIC ns).
// CEP:WHY: The jitdump format mandates CLOCK_MONOTONIC; one source keeps the
//          jitdump, sampler, and safepoint rings comparable.
// CEP:STATUS: complete
// CEP:FAILURE: none (a failed clock read returns the errno-tainted 0 — the
//          writer surfaces the anomaly through its timestamp contract tests).
// CEP:ASSUMES: Linux CLOCK_MONOTONIC (the M6-b platform contract).
// CEP:COST: @cold/@warm. One vDSO clock_gettime.
// CEP:EVIDENCE: every timestamp DoD test (tests/test_observability.cpp).
uint64_t mono_clock_ns() noexcept {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

// ---- JitDumpWriter -------------------------------------------------------------

JitDumpWriter::~JitDumpWriter() {
    if (open_) (void)close();
}

// CEP:WHAT: Move constructor: transfers the fd, the self-mmap, and the
//           byte counter; the source becomes closed.
// CEP:WHY: Writers own OS resources — move-only keeps one owner (the
//          JitSession holds it).
// CEP:STATUS: complete
// CEP:FAILURE: none (the source is reset, never closed).
// CEP:ASSUMES: noexcept (resource handles only).
// CEP:COST: @cold. Plain field moves.
// CEP:EVIDENCE: the session/tests move writers by value paths.
JitDumpWriter::JitDumpWriter(JitDumpWriter&& other) noexcept
    : fd_(other.fd_),
      mmap_base_(other.mmap_base_),
      mmap_bytes_(other.mmap_bytes_),
      bytes_written_(other.bytes_written_),
      open_(other.open_) {
    other.fd_ = -1;
    other.mmap_base_ = nullptr;
    other.mmap_bytes_ = 0;
    other.bytes_written_ = 0;
    other.open_ = false;
}

// CEP:WHAT: Move assignment: closes any owned resources first, then
//           transfers; self-assignment is a no-op.
// CEP:WHY: The move-constructor contract at assignment position.
// CEP:STATUS: complete
// CEP:FAILURE: the pre-transfer close is best-effort (destruct-or path).
// CEP:ASSUMES: noexcept (resource handles only).
// CEP:COST: @cold. Plain field moves.
// CEP:EVIDENCE: exercised by session reassignment paths.
JitDumpWriter& JitDumpWriter::operator=(JitDumpWriter&& other) noexcept {
    if (this != &other) {
        if (open_) (void)close();
        fd_ = other.fd_;
        mmap_base_ = other.mmap_base_;
        mmap_bytes_ = other.mmap_bytes_;
        bytes_written_ = other.bytes_written_;
        open_ = other.open_;
        other.fd_ = -1;
        other.mmap_base_ = nullptr;
        other.mmap_bytes_ = 0;
        other.bytes_written_ = 0;
        other.open_ = false;
    }
    return *this;
}

// CEP:WHAT: Writes one buffer with a loop (short writes are normal I/O, not
//           errors).
// CEP:WHY: The jitdump records must land whole — a partial write silently
//           read back by perf would corrupt the stream (Rule 76: no silent
//           truncation).
// CEP:STATUS: complete
// CEP:FAILURE: -1/EIO-class errors -> named RuntimeError.
// CEP:ASSUMES: A regular file fd (not a pipe — the format is a file).
// CEP:COST: @cold.
// CEP:EVIDENCE: the parse-back DoD tests read every byte back.
Result<void> JitDumpWriter::write_raw(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t left = bytes;
    while (left > 0) {
        const ssize_t n = ::write(fd_, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return support::fail(support::ErrorCode::RuntimeError,
                                 "observability: jitdump write failed: " +
                                     std::string(std::strerror(errno)));
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    bytes_written_ += bytes;
    return support::ok();
}

// CEP:WHAT: Appends one little-endian u32 through the short-write loop.
// CEP:WHY: The record framing's id/size fields (docs/infrastructure/06
//          section 2).
// CEP:STATUS: complete
// CEP:FAILURE: write failures surface through write_raw (named I/O errors).
// CEP:ASSUMES: an open writer (callers checked).
// CEP:COST: @cold. One buffer + one write.
// CEP:EVIDENCE: the DoD parse-backs read these fields.
Result<void> JitDumpWriter::write_u32(uint32_t value) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    return write_raw(b, 4);
}

// CEP:WHAT: Appends one little-endian u64 through the short-write loop.
// CEP:WHY: The record framing's timestamp + the code-range fields.
// CEP:STATUS: complete
// CEP:FAILURE: write failures surface through write_raw (named I/O errors).
// CEP:ASSUMES: an open writer (callers checked).
// CEP:COST: @cold. One buffer + one write.
// CEP:EVIDENCE: the DoD parse-backs read these fields.
Result<void> JitDumpWriter::write_u64(uint64_t value) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    return write_raw(b, 8);
}

// CEP:WHAT: Appends the NUL-terminated name through the short-write loop.
// CEP:WHY: CODE_LOAD names are NUL-terminated per the v1 spec.
// CEP:STATUS: complete
// CEP:FAILURE: write failures surface through write_raw (named I/O errors).
// CEP:ASSUMES: an open writer (callers checked).
// CEP:COST: @cold. One write.
// CEP:EVIDENCE: DoD 2 asserts the exact name bytes + the NUL.
Result<void> JitDumpWriter::write_name(const std::string& name) {
    return write_raw(name.data(), name.size() + 1);  // includes the NUL
}

// CEP:WHAT: The open-writer precondition: a named InvalidState reject when
//           the API is misused after close.
// CEP:WHY: Rule 76 — use-after-stop is a named failure, never silence.
// CEP:STATUS: complete
// CEP:FAILURE: the named reject itself (closed writer).
// CEP:ASSUMES: `what` names the API for the diagnostic.
// CEP:COST: @cold. One flag load.
// CEP:EVIDENCE: DoD 6 pins the message shape.
Result<void> JitDumpWriter::require_open(const char* what) const {
    if (!open_) {
        return support::fail(
            support::ErrorCode::InvalidState,
            std::string("observability: jitdump ") + what +
                " on a closed writer (open() first)");
    }
    return support::ok();
}

// CEP:WHAT: Creates/truncates the jitdump file, writes the 40-byte v1 header,
//           installs the PROT_READ|PROT_EXEC self-mmap over the header
//           extent (perf's discovery beacon).
// CEP:WHY: docs/infrastructure/06 section 2 — the normative header layout
//          and the self-mmap requirement.
// CEP:STATUS: complete
// CEP:FAILURE: open/creation failures and mmap failure -> named errors; a
//          header write failure closes the fd before returning.
// CEP:ASSUMES: One writer per process (JitSession enforces).
// CEP:COST: @cold. One open + one write + one mmap.
// CEP:EVIDENCE: DoD 1 (header bytes) and DoD 7 (the mapping probe).
Result<void> JitDumpWriter::open(const std::string& path) {
    if (open_) {
        return support::fail(support::ErrorCode::InvalidState,
                             "observability: jitdump open while already open");
    }
    if (path.empty()) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "observability: jitdump open with an empty path");
    }
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        return support::fail(support::ErrorCode::RuntimeError,
                             "observability: cannot create jitdump '" + path +
                                 "': " + std::strerror(errno));
    }
    bytes_written_ = 0;
    std::vector<uint8_t> header;
    header.reserve(kJitdumpHeaderSize);
    push_u32(header, kJitdumpMagic);
    push_u32(header, kJitdumpVersion);
    push_u32(header, kJitdumpHeaderSize);
    push_u32(header, kJitdumpElfMach);
    push_u32(header, 0);  // pad1
    push_u32(header, static_cast<uint32_t>(::getpid()));
    push_u64(header, mono_clock_ns());
    push_u64(header, 0);  // flags
    if (auto r = write_raw(header.data(), header.size()); !r) {
        ::close(fd_);
        fd_ = -1;
        return r;
    }
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        ::close(fd_);
        fd_ = -1;
        return support::fail(support::ErrorCode::Unsupported,
                             "observability: sysconf(_SC_PAGESIZE) failed");
    }
    mmap_bytes_ =
        (static_cast<size_t>(kJitdumpHeaderSize) + static_cast<size_t>(page) - 1) &
        ~(static_cast<size_t>(page) - 1);
    mmap_base_ = ::mmap(nullptr, mmap_bytes_, PROT_READ | PROT_EXEC, MAP_SHARED,
                        fd_, 0);
    if (mmap_base_ == MAP_FAILED) {
        mmap_base_ = nullptr;
        mmap_bytes_ = 0;
        ::close(fd_);
        fd_ = -1;
        return support::fail(support::ErrorCode::Unsupported,
                             "observability: jitdump self-mmap failed: " +
                                 std::string(std::strerror(errno)));
    }
    open_ = true;
    return support::ok();
}

// CEP:WHAT: JIT_CODE_LOAD: framing (id/size/timestamp) + pid/tid + the code
//           range + code_index + the NUL-terminated name + nr_debug_entries.
// CEP:WHY: The perf attribution record (docs/infrastructure/06 section 2).
// CEP:STATUS: complete
// CEP:FAILURE: closed-writer use -> named reject; non-empty debug spans ->
//          a named batch-e refusal (the deferred-work error class; real
//          entries are batch e, the field itself is written zero-filled);
//          write failures -> named errors.
// CEP:ASSUMES: The record is assembled in one buffer and written with one
//          write_raw so a reader never sees a torn record.
// CEP:COST: @cold. One buffer + one write syscall.
// CEP:EVIDENCE: DoD 2/3 (parse-back + monotonic timestamps).
Result<void> JitDumpWriter::record_code_load(uint64_t vma, uint64_t code_addr,
                                             uint64_t code_size,
                                             uint64_t code_index,
                                             const std::string& name,
                                             std::span<const DebugEntry> debug) {
    if (auto r = require_open("record_code_load"); !r) return r;
    if (!debug.empty()) {
        return support::fail(support::ErrorCode::Unimplemented,
                             "observability: jitdump debug entries are batch e "
                             "(nr_debug_entries stays 0 in M6-d)");
    }
    const uint64_t ts = mono_clock_ns();
    // size = framing(16) + pid/tid(8) + code fields(32) + name(+NUL) +
    //        nr_debug_entries(4)
    uint32_t size = 16 + 8 + 32 + 4 + static_cast<uint32_t>(name.size()) + 1;
    std::vector<uint8_t> rec;
    rec.reserve(size);
    push_u32(rec, kJitCodeLoadId);
    push_u32(rec, size);
    push_u64(rec, ts);
    push_u32(rec, static_cast<uint32_t>(::getpid()));
    push_u32(rec, static_cast<uint32_t>(::syscall(SYS_gettid)));
    push_u64(rec, vma);
    push_u64(rec, code_addr);
    push_u64(rec, code_size);
    push_u64(rec, code_index);
    push_name(rec, name);
    push_u32(rec, 0);  // nr_debug_entries (reserved; batch e)
    return write_raw(rec.data(), rec.size());
}

// CEP:WHAT: JIT_CODE_CLOSE framing record.
// CEP:WHY: Marks the producer's end so perf can bound its parse.
// CEP:STATUS: complete
// CEP:FAILURE: closed-writer use -> named reject; write failure -> named.
// CEP:ASSUMES: none beyond an open writer.
// CEP:COST: @cold. One 16-byte write.
// CEP:EVIDENCE: DoD 4 (a stopped session parses back completely).
Result<void> JitDumpWriter::record_close() {
    if (auto r = require_open("record_close"); !r) return r;
    std::vector<uint8_t> rec;
    push_u32(rec, kJitCodeCloseId);
    push_u32(rec, 16);
    push_u64(rec, mono_clock_ns());
    return write_raw(rec.data(), rec.size());
}

// CEP:WHAT: CODE_CLOSE + munmap + fd close; the writer leaves the file
//           complete on disk (perf reads it after process exit).
// CEP:WHY: Session-stop semantics (docs/infrastructure/06 section 2).
// CEP:STATUS: complete
// CEP:FAILURE: double close -> named reject; a CODE_CLOSE write failure is
//          returned but the fd/mapping are still released (close must close).
// CEP:ASSUMES: none.
// CEP:COST: @cold. One write + munmap + close.
// CEP:EVIDENCE: DoD 4/6.
Result<void> JitDumpWriter::close() {
    if (!open_) {
        return support::fail(support::ErrorCode::InvalidState,
                             "observability: jitdump close on a closed writer");
    }
    // CODE_CLOSE first (record_close requires the open flag), THEN the flag
    // drops and the fd/mapping are released — close must close either way.
    Result<void> result = record_close();
    open_ = false;
    if (mmap_base_ != nullptr) {
        ::munmap(mmap_base_, mmap_bytes_);
        mmap_base_ = nullptr;
        mmap_bytes_ = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    return result;
}

// ---- PerfMap -------------------------------------------------------------------

PerfMap::~PerfMap() {
    if (open_) (void)close();
}

// CEP:WHAT: Move constructor: transfers the map fd; the source closes.
// CEP:WHY: Maps own an fd — move-only keeps one owner (JitSession holds it).
// CEP:STATUS: complete
// CEP:FAILURE: none (the source is reset, never closed).
// CEP:ASSUMES: noexcept (resource handles only).
// CEP:COST: @cold. Plain field moves.
// CEP:EVIDENCE: exercised by session/test paths.
PerfMap::PerfMap(PerfMap&& other) noexcept : fd_(other.fd_), open_(other.open_) {
    other.fd_ = -1;
    other.open_ = false;
}

// CEP:WHAT: Move assignment: closes any owned fd first, then transfers;
//           self-assignment is a no-op.
// CEP:WHY: The move-constructor contract at assignment position.
// CEP:STATUS: complete
// CEP:FAILURE: the pre-transfer close is best-effort (destruct-or path).
// CEP:ASSUMES: noexcept (resource handles only).
// CEP:COST: @cold. Plain field moves.
// CEP:EVIDENCE: exercised by session/test paths.
PerfMap& PerfMap::operator=(PerfMap&& other) noexcept {
    if (this != &other) {
        if (open_) (void)close();
        fd_ = other.fd_;
        open_ = other.open_;
        other.fd_ = -1;
        other.open_ = false;
    }
    return *this;
}

// CEP:WHAT: Opens the perf-map sidecar (create/truncate, 0644).
// CEP:WHY: perf's fallback symbolizer (docs/infrastructure/06 section 3).
// CEP:STATUS: complete
// CEP:FAILURE: uncreatable path / already open / empty path -> named errors.
// CEP:ASSUMES: One map writer per process (JitSession enforces).
// CEP:COST: @cold. One open.
// CEP:EVIDENCE: DoD 8/9.
Result<void> PerfMap::open(const std::string& path) {
    if (open_) {
        return support::fail(support::ErrorCode::InvalidState,
                             "observability: perf-map open while already open");
    }
    if (path.empty()) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "observability: perf-map open with an empty path");
    }
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        return support::fail(support::ErrorCode::RuntimeError,
                             "observability: cannot create perf-map '" + path +
                                 "': " + std::strerror(errno));
    }
    open_ = true;
    return support::ok();
}

// CEP:WHAT: Appends "<hex_start> <hex_size> <name>\n" with the name
//           sanitized (space/newline/tab -> '_') — the format is
//           whitespace-delimited.
// CEP:WHY: HotSpot's sanitizer is the precedent (docs/infrastructure/06
//          section 3).
// CEP:STATUS: complete
// CEP:FAILURE: closed-map use -> named reject; write failure -> named.
// CEP:ASSUMES: lowercase hex WITHOUT the 0x prefix.
// CEP:COST: @cold. One formatted write.
// CEP:EVIDENCE: DoD 8 (format + sanitization).
Result<void> PerfMap::record(uint64_t start, uint64_t size,
                             const std::string& name) {
    if (!open_) {
        return support::fail(support::ErrorCode::InvalidState,
                             "observability: perf-map record on a closed map");
    }
    std::string sanitized;
    sanitized.reserve(name.size());
    for (const char c : name) {
        sanitized.push_back(
            (c == ' ' || c == '\n' || c == '\t' || c == '\r') ? '_' : c);
    }
    char head[48];
    const int n = ::snprintf(head, sizeof(head), "%zx %zx ", start, size);
    if (n <= 0) {
        return support::fail(support::ErrorCode::InternalError,
                             "observability: perf-map header format failed");
    }
    std::vector<uint8_t> line;
    line.reserve(static_cast<size_t>(n) + sanitized.size() + 1);
    line.insert(line.end(), head, head + n);
    line.insert(line.end(), sanitized.begin(), sanitized.end());
    line.push_back(static_cast<uint8_t>('\n'));
    size_t left = line.size();
    const uint8_t* p = line.data();
    while (left > 0) {
        const ssize_t w = ::write(fd_, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            return support::fail(support::ErrorCode::RuntimeError,
                                 "observability: perf-map write failed: " +
                                     std::string(std::strerror(errno)));
        }
        p += w;
        left -= static_cast<size_t>(w);
    }
    return support::ok();
}

// CEP:WHAT: Closes the map fd; the file persists (perf reads it after
//           process exit).
// CEP:WHY: Session-stop semantics on the sidecar half.
// CEP:STATUS: complete
// CEP:FAILURE: double close -> named reject.
// CEP:ASSUMES: none beyond an open map.
// CEP:COST: @cold. One close.
// CEP:EVIDENCE: DoD 8/10.
Result<void> PerfMap::close() {
    if (!open_) {
        return support::fail(support::ErrorCode::InvalidState,
                             "observability: perf-map close on a closed map");
    }
    open_ = false;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    return support::ok();
}

// ---- JitSession ----------------------------------------------------------------

// CEP:WHAT: The live-run default paths: /tmp/perf-<pid>.jitdump and
//           /tmp/perf-<pid>.map (the convention perf scans).
// CEP:WHY: docs/infrastructure/06 section 2/3 — the discovery convention.
// CEP:STATUS: complete
// CEP:FAILURE: none (path construction only).
// CEP:ASSUMES: /tmp is the live-run convention (explicit paths override).
// CEP:COST: @cold. One getpid + one to_string.
// CEP:EVIDENCE: start_default tests + the vx --jitdump flag.
std::string JitSession::default_jitdump_path() {
    return "/tmp/perf-" + std::to_string(::getpid()) + ".jitdump";
}

// CEP:WHAT: The perf-map half of the default path pair.
// CEP:WHY: Same convention as default_jitdump_path.
// CEP:STATUS: complete
// CEP:FAILURE: none (path construction only).
// CEP:ASSUMES: /tmp is the live-run convention (explicit paths override).
// CEP:COST: @cold. One getpid + one to_string.
// CEP:EVIDENCE: start_default tests + the vx --jitdump flag.
std::string JitSession::default_perf_map_path() {
    return "/tmp/perf-" + std::to_string(::getpid()) + ".map";
}

// CEP:WHAT: Opens both telemetry sides; a second-side failure rolls the
//           first back so a session is never half-open.
// CEP:WHY: The one-producer-per-process contract (docs/infrastructure/06
//          section 4).
// CEP:STATUS: complete
// CEP:FAILURE: double-open -> named reject; uncreatable paths -> named
//          errors (the jitdump side closed on rollback).
// CEP:ASSUMES: Driver-thread calls.
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 5/10.
Result<void> JitSession::start(const std::string& jitdump_path,
                               const std::string& perf_map_path) {
    if (active_) {
        return support::fail(support::ErrorCode::InvalidState,
                             "observability: session already active (one "
                             "jitdump producer per process)");
    }
    if (auto r = jitdump_.open(jitdump_path); !r) return r;
    if (auto r = perf_map_.open(perf_map_path); !r) {
        (void)jitdump_.close();
        return r;
    }
    code_index_ = 0;
    code_loads_ = 0;
    last_error_.clear();
    active_ = true;
    return support::ok();
}

// CEP:WHAT: CODE_CLOSE + closes both writers; the files persist (perf reads
//           them after process exit).
// CEP:WHY: Session-stop semantics; both sides must reach a complete state.
// CEP:STATUS: complete
// CEP:FAILURE: stop on an inactive session -> named reject; a side's close
//          failure is returned (both sides still close).
// CEP:ASSUMES: Driver-thread calls.
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 4/5.
Result<void> JitSession::stop() {
    if (!active_) {
        return support::fail(support::ErrorCode::InvalidState,
                             "observability: stop on an inactive session");
    }
    active_ = false;
    Result<void> result = jitdump_.close();
    if (auto r = perf_map_.close(); !r && result) result = r;
    return result;
}

// CEP:WHAT: The publish hook: one CODE_LOAD + one perf-map line per code
//           range, synthetic stable symbol "vx_<tier>_m<method_id>". Never
//           fails the caller — errors land in last_error() (telemetry must
//           never reject correctly-compiled code).
// CEP:WHY: The publish path's only touchpoint (one active() branch when
//          idle); Rule 120 — the stored error is the observable.
// CEP:STATUS: complete
// CEP:FAILURE: none to the caller (all failures stored); allocation is
//          contained in a try/catch so the noexcept contract holds.
// CEP:ASSUMES: code_base/code_bytes describe a live, final code range (the
//          publish-path discipline).
// CEP:COST: @cold. One branch when idle; two writes per record.
// CEP:EVIDENCE: DoD 9/10 (record matching + the telemetry-failure publish).
void JitSession::record_code(const void* code_base, size_t code_bytes,
                             uint32_t method_id, const char* tier) noexcept {
    if (!active_) return;
    try {
        char name[64];
        const int n = ::snprintf(name, sizeof(name), "vx_%s_m%u",
                                 tier != nullptr ? tier : "j", method_id);
        if (n <= 0) {
            last_error_ = "observability: symbol format failed";
            return;
        }
        const uint64_t base = reinterpret_cast<uint64_t>(code_base);
        if (auto r = jitdump_.record_code_load(base, base, code_bytes,
                                               code_index_, std::string(name));
            !r) {
            last_error_ = r.error().format();
            return;
        }
        ++code_index_;
        if (auto r = perf_map_.record(base, code_bytes, std::string(name));
            !r) {
            last_error_ = r.error().format();
            return;
        }
        ++code_loads_;
    } catch (const std::exception& e) {
        try {
            last_error_ = std::string("observability: record_code: ") + e.what();
        } catch (...) {
            last_error_.clear();
        }
    } catch (...) {
        try {
            last_error_ = "observability: record_code unknown failure";
        } catch (...) {
            last_error_.clear();
        }
    }
}

// CEP:WHAT: The session accessor (function-local static — thread-safe
//           magic-static init, the gc_code_registry shape).
// CEP:WHY: One producer per process; publish sites take one branch.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: none.
// CEP:COST: @cold. One guard load after first init.
// CEP:EVIDENCE: consumed by the J1/J2 publish sites; DoD 9/10.
JitSession& jit_session() noexcept {
    static JitSession session;
    return session;
}

// ---- SafepointLog --------------------------------------------------------------

// CEP:WHAT: Records one suspension event; overflow CLEARS the ring (Rule 28,
//           the GC pause ring's discipline) while total_events_ keeps the
//           true count. The push is guarded: an allocation failure DEGRADES
//           (the event is dropped) — record() never propagates a failure
//           into the handshake manager's critical sections.
// CEP:WHY: Bounded telemetry with honest counters (Rule 120); the log must
//          never fail the handshake (docs/infrastructure/06 section 5).
// CEP:STATUS: complete
// CEP:FAILURE: none — overflow is the documented clear; an allocation
//          failure drops the event and keeps the counters honest.
// CEP:ASSUMES: Called under the handshake manager's serialization or from
//          the single driver thread.
// CEP:COST: @cold. One push.
// CEP:EVIDENCE: DoD 11-14.
void SafepointLog::record(SafepointEvent event) {
    if (events_.size() >= kMaxEvents) events_.clear();
    try {
        events_.push_back(event);
    } catch (...) {
        return;  // degrade: drop the event, never propagate into the caller
    }
    ++total_events_;
}

// CEP:WHAT: Nearest-rank percentile over the ring's ttsp values (the exact
//           GcStats::pause_percentile_ns shape — the two p99 numbers must be
//           computed identically).
// CEP:WHY: The measurement plan's TTSP p99 gate.
// CEP:STATUS: complete
// CEP:FAILURE: 0 for an empty ring.
// CEP:ASSUMES: the ring is stable during the call.
// CEP:COST: @cold. O(n log n) over <= 512 events.
// CEP:EVIDENCE: DoD 11-13.
uint64_t SafepointLog::ttsp_percentile_ns(unsigned pct) const {
    if (events_.empty()) return 0;
    std::vector<uint64_t> ttsp;
    ttsp.reserve(events_.size());
    for (const SafepointEvent& e : events_) ttsp.push_back(e.ttsp_ns);
    std::sort(ttsp.begin(), ttsp.end());
    const size_t rank = (static_cast<size_t>(pct) * ttsp.size() + 99) / 100;
    return ttsp[std::min(rank, ttsp.size()) - 1];
}

// CEP:WHAT: Maximum recorded ttsp.
// CEP:WHY: Diagnostics alongside the percentile.
// CEP:STATUS: complete
// CEP:FAILURE: 0 for an empty ring.
// CEP:ASSUMES: the ring is stable during the call.
// CEP:COST: @cold. O(n).
// CEP:EVIDENCE: DoD 11-13.
uint64_t SafepointLog::ttsp_max_ns() const {
    uint64_t m = 0;
    for (const SafepointEvent& e : events_) m = std::max(m, e.ttsp_ns);
    return m;
}

// ---- AllocSampler --------------------------------------------------------------

// CEP:WHAT: Sets the sampling interval; 0 is a named reject (an interval of
//           zero would sample every refill — a config error, not a mode).
//           The interval change starts a FRESH accumulation window (the
//           residue of the old interval is an implementation detail — a new
//           config must not fire samples for bytes counted under the old
//           one).
// CEP:WHY: Rule 76 — configuration misuse is named, never silently clamped.
// CEP:STATUS: complete
// CEP:FAILURE: 0 -> named InvalidArgument.
// CEP:ASSUMES: Driver-thread configuration (before or between runs).
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 16 (exact per-window deltas).
Result<void> AllocSampler::set_interval_bytes(uint64_t interval) {
    if (interval == 0) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "observability: alloc sampler interval must be "
                             "nonzero");
    }
    interval_bytes_ = interval;
    accumulated_bytes_ = 0;  // a new interval starts a fresh window
    return support::ok();
}

// CEP:WHAT: The Heap::refill_tlab hook: one add + compare steady-state; each
//           interval crossing records one sample (timestamp, refill bytes,
//           cumulative bytes, site 0 = unknown). Ring clears at capacity;
//           counters never do (Rule 120). The sample push is guarded: an
//           allocation failure drops the sample — the @warm refill path
//           must never propagate into the allocation slow path.
// CEP:WHY: JEP-331-shape allocation profiling at near-zero steady-state
//          cost — the WARM refill path only, never the hot bump.
// CEP:STATUS: complete
// CEP:FAILURE: none (config misuse rejects by name via set_interval_bytes;
//          an allocation failure during a sample push degrades silently).
// CEP:ASSUMES: The single mutator calls this (Rule 83); attribution is the
//          emitted-poll batch's upgrade (site_id reserved 0).
// CEP:COST: @warm. One add + one compare steady-state; one ring push per
//          interval crossing.
// CEP:EVIDENCE: DoD 16 (interval exactness, ring bound, fast-path
//          independence).
void AllocSampler::on_refill(uint64_t refill_bytes) noexcept {
    ++refills_observed_;
    total_refill_bytes_ += refill_bytes;
    accumulated_bytes_ += refill_bytes;
    while (accumulated_bytes_ >= interval_bytes_) {
        accumulated_bytes_ -= interval_bytes_;
        if (samples_.size() >= kMaxSamples) samples_.clear();
        AllocSample sample;
        sample.timestamp_ns = mono_clock_ns();
        sample.refill_bytes = refill_bytes;
        sample.cumulative_bytes = total_refill_bytes_;
        sample.site_id = 0;
        try {
            samples_.push_back(sample);
        } catch (...) {
            return;  // degrade: drop the sample, keep the counters honest
        }
        ++total_samples_;
    }
}

// CEP:WHAT: The sampler accessor (function-local static, the
//          gc_code_registry shape).
// CEP:WHY: Heap::refill_tlab and `vx stats` share one instance.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: none.
// CEP:COST: @cold/@warm. One guard load after first init.
// CEP:EVIDENCE: DoD 16/17.
AllocSampler& alloc_sampler() noexcept {
    static AllocSampler sampler;
    return sampler;
}

// ---- batch-e contract stubs ----------------------------------------------------

// CEP:WHAT: The GDB JIT interface registration entry (contract stub).
// CEP:WHY: docs/infrastructure/06 section 8 — the debugger leg of the
//          observability item, deliberately not in the M6-d floor.
// CEP:CLASS: CEP-2
// CEP:STATUS: stub
// CEP:OWNER: M6 infra-6 batch e
// CEP:TICKET: docs/roadmap.md M6 observability item (GDB JIT interface,
//          managed sanitizers, deterministic replay)
// CEP:FAILURE: named refusal on every call (the batch-e work replaces it).
// CEP:ASSUMES: no caller in the M6-d floor depends on it.
// CEP:COST: @cold. One named error return.
// CEP:EVIDENCE: the roadmap bullet tracks the deferred mechanisms.
Result<void> GdbJitInterface::register_code(const void* code,
                                            size_t size,
                                            const char* symbol) {
    (void)code;
    (void)size;
    (void)symbol;
    return support::unimplemented("GDB JIT interface (batch e)");
}

// CEP:WHAT: The GDB JIT interface unregistration entry (contract stub).
// CEP:WHY: Paired with register_code under the same batch-e ticket.
// CEP:CLASS: CEP-2
// CEP:STATUS: stub
// CEP:OWNER: M6 infra-6 batch e
// CEP:TICKET: docs/roadmap.md M6 observability item (GDB JIT interface,
//          managed sanitizers, deterministic replay)
// CEP:FAILURE: named refusal on every call (the batch-e work replaces it).
// CEP:ASSUMES: no caller in the M6-d floor depends on it.
// CEP:COST: @cold. One named error return.
// CEP:EVIDENCE: the roadmap bullet tracks the deferred mechanisms.
Result<void> GdbJitInterface::unregister_code(const void* code) {
    (void)code;
    return support::unimplemented("GDB JIT interface (batch e)");
}

void ReplayRecorder::record_event(uint32_t kind, uint64_t payload) {
    log_.emplace_back(kind, payload);
}

}  // namespace vortex::infra
