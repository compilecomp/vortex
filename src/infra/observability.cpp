// CEP:FILE: src/infra/observability.cpp
// CEP:WHAT: The observability floor's implementation: the perf-jitdump v1
//           writer (packed unaligned records, CLOCK_MONOTONIC timestamps,
//           the PROT_EXEC self-mmap), the perf-map sidecar, the JitSession
//           singleton facade with best-effort publish hooks, the safepoint
//           TTSP ring, and the TLAB-refill allocation sampler; batch e
//           completes the item (GDB JIT interface + ELF64 symfiles, the
//           guest-heap shadow, the replay ring + differ).
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
// CEP:EVIDENCE: tests/test_observability.cpp — the DoD 1-33 parse-backs,
//          probe, ring bounds, and named-reject pins.
#include "vortex/infra/observability.hpp"

#include <elf.h>
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
#include <mutex>
#include <vector>

namespace vortex::infra {

using support::ok;

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
    const uint64_t ts = mono_clock_ns();
    // size = framing(16) + pid/tid(8) + code fields(32) + name(+NUL) +
    //        nr_debug_entries(4) + entries(16 each: u64 addr, u32 line,
    //        u32 column)
    uint32_t size = 16 + 8 + 32 + 4 + static_cast<uint32_t>(name.size()) + 1 +
                    static_cast<uint32_t>(debug.size()) * 16;
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
    push_u32(rec, static_cast<uint32_t>(debug.size()));
    for (const DebugEntry& d : debug) {
        push_u64(rec, d.addr);
        push_u32(rec, d.line);
        push_u32(rec, d.discriminant);
    }
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
        ++code_loads_;  // COMPLETE publications only (both sides landed —
        // the consumer-visible unit is the pair; Rule 120 honesty, DoD 10).
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

// ---- batch e: GDB JIT interface -------------------------------------------------

// CEP:WHAT: The debugger-visible descriptor definition (version 1, empty
//           chain) and the breakpoint target — the two symbols gdb/jit.h
//           requires at the exact C-linkage spellings.
// CEP:WHY: docs/infrastructure/06 GDB JIT interface section — the ABI IS
//          the contract; the debugger reads these names at attach time.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Defined exactly once; static zero-init except version.
// CEP:COST: @cold. Static storage + one ret.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 18.
extern "C" __attribute__((used)) GdbJitDescriptor __jit_debug_descriptor = {
    1, 0, nullptr, nullptr};

extern "C" __attribute__((used, noinline)) void __jit_debug_register_code(
    void) noexcept {
    // Empty by contract: the breakpoint IS the effect (gdb/jit.h ABI).
}

namespace {

// CEP:WHAT: One live GDB registration: the descriptor-list entry, its
//           symfile image (the byte snapshot), and the registered base.
// CEP:WHY: The registration owns the symfile the descriptor points at —
//          the entry's addresses must outlive the registration.
// CEP:STATUS: complete
// CEP:FAILURE: none (owned state; moved only).
// CEP:ASSUMES: Mutated only under GdbState::mutex.
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 19-22.
struct GdbRegistration {
    std::unique_ptr<GdbJitCodeEntry> entry;
    std::vector<uint8_t> symfile;
    uint64_t code_addr = 0;
};

// CEP:WHAT: The interface state: the live registrations + the mutex that
//           serializes descriptor/chain mutation.
// CEP:WHY: The descriptor is process-global debugger-visible state — one
//          writer at a time (registration is @cold; never signal context).
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Not a hot tree (@cold publish path — the CEM-26 s7 mutex
//          ban scopes hot trees).
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 21 (serialized named rejects).
struct GdbState {
    std::mutex mutex;
    std::vector<GdbRegistration> live;
};

GdbState& gdb_state() noexcept {
    static GdbState state;
    return state;
}

// CEP:WHAT: Builds the deterministic in-memory ELF64 ET_EXEC symfile:
//           [Ehdr][Phdr PT_LOAD R+X][.text byte snapshot][.symtab: one
//           STB_GLOBAL/STT_FUNC at the absolute code_addr][.strtab]
//           [.shstrtab][5 section headers]. Fixed layout, zero padding,
//           no timestamps — byte-identical for identical inputs.
// CEP:WHY: gdb parses the symfile to name JIT frames; determinism makes
//          the emitter golden-pinnable (the same discipline as the M6-a
//          artifacts).
// CEP:STATUS: complete
// CEP:FAILURE: Throws std::bad_alloc on OOM (callers translate to the
//          named OutOfMemory reject — nothing is mutated before the build
//          completes).
// CEP:ASSUMES: LP64; <elf.h> layout constants (the platform ABI).
// CEP:COST: @cold. One image-sized allocation + copies.
// CEP:EVIDENCE: DoD 19/22 (parse-back, multi-entry).
std::vector<uint8_t> build_gdb_symfile(uint64_t code_addr,
                                       const uint8_t* code, size_t size,
                                       const char* symbol) {
    // shstrtab name offsets (fixed string layout below).
    constexpr size_t kNameText = 1;      // ".text"
    constexpr size_t kNameSymtab = 7;    // ".symtab"
    constexpr size_t kNameStrtab = 15;   // ".strtab"
    constexpr size_t kNameShstrtab = 23; // ".shstrtab"
    static const char kShstrtab[] =
        "\0.text\0.symtab\0.strtab\0.shstrtab\0";
    const size_t shstr_size = sizeof(kShstrtab);  // includes final NUL

    const std::string sym = symbol != nullptr ? symbol : "";
    const size_t str_size = 1 + sym.size() + 1;  // leading NUL + name + NUL

    size_t off = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
    const size_t text_off = off;
    off = text_off + size;
    const size_t sym_off = (off + 7) & ~size_t(7);
    off = sym_off + 2 * sizeof(Elf64_Sym);
    const size_t str_off = off;
    off = str_off + str_size;
    const size_t shstr_off = off;
    off = shstr_off + shstr_size;
    const size_t sh_off = (off + 7) & ~size_t(7);
    const size_t total = sh_off + 5 * sizeof(Elf64_Shdr);

    std::vector<uint8_t> img(total, 0);
    const auto put = [&](size_t at, const void* p, size_t n) {
        std::memcpy(img.data() + at, p, n);
    };

    Elf64_Ehdr eh{};
    eh.e_ident[0] = 0x7f;
    eh.e_ident[1] = 'E';
    eh.e_ident[2] = 'L';
    eh.e_ident[3] = 'F';
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_EXEC;
    eh.e_machine = EM_X86_64;
    eh.e_version = EV_CURRENT;
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_shoff = sh_off;
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = 1;
    eh.e_shentsize = sizeof(Elf64_Shdr);
    eh.e_shnum = 5;
    eh.e_shstrndx = 4;
    put(0, &eh, sizeof eh);

    Elf64_Phdr ph{};
    ph.p_type = PT_LOAD;
    ph.p_flags = PF_R | PF_X;
    ph.p_offset = 0;
    ph.p_vaddr = 0;
    ph.p_paddr = 0;
    ph.p_filesz = total;
    ph.p_memsz = total;
    ph.p_align = 0x1000;
    put(sizeof(Elf64_Ehdr), &ph, sizeof ph);

    put(text_off, code, size);

    Elf64_Sym syms[2]{};
    syms[1].st_name = 1;  // ".strtab" byte 1 = the symbol name
    syms[1].st_info = static_cast<uint8_t>((STB_GLOBAL << 4) | STT_FUNC);
    syms[1].st_shndx = 1;  // .text
    syms[1].st_value = code_addr;
    syms[1].st_size = size;
    put(sym_off, syms, sizeof syms);

    img[str_off] = 0;
    if (!sym.empty())
        std::memcpy(img.data() + str_off + 1, sym.data(), sym.size());
    img[str_off + 1 + sym.size()] = 0;

    std::memcpy(img.data() + shstr_off, kShstrtab, shstr_size);

    Elf64_Shdr sh[5]{};
    sh[1].sh_name = static_cast<uint32_t>(kNameText);
    sh[1].sh_type = SHT_PROGBITS;
    sh[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    sh[1].sh_addr = code_addr;
    sh[1].sh_offset = text_off;
    sh[1].sh_size = size;
    sh[1].sh_addralign = 16;
    sh[2].sh_name = static_cast<uint32_t>(kNameSymtab);
    sh[2].sh_type = SHT_SYMTAB;
    sh[2].sh_offset = sym_off;
    sh[2].sh_size = 2 * sizeof(Elf64_Sym);
    sh[2].sh_link = 3;  // .strtab
    sh[2].sh_info = 1;  // one local (the null symbol)
    sh[2].sh_entsize = sizeof(Elf64_Sym);
    sh[2].sh_addralign = 8;
    sh[3].sh_name = static_cast<uint32_t>(kNameStrtab);
    sh[3].sh_type = SHT_STRTAB;
    sh[3].sh_offset = str_off;
    sh[3].sh_size = str_size;
    sh[3].sh_addralign = 1;
    sh[4].sh_name = static_cast<uint32_t>(kNameShstrtab);
    sh[4].sh_type = SHT_STRTAB;
    sh[4].sh_offset = shstr_off;
    sh[4].sh_size = shstr_size;
    sh[4].sh_addralign = 1;
    put(sh_off, sh, sizeof sh);

    return img;
}

}  // namespace

// CEP:WHAT: register_code — validates, builds the symfile snapshot, links
//           the entry at the descriptor chain head, signals via the
//           breakpoint function. Named rejects: null/empty, duplicate live
//           base, OOM (translated from bad_alloc — nothing mutated before
//           the snapshot completes).
// CEP:WHY: The interface's registration half (docs/infrastructure/06).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: the named rejects above; mutex-serialized.
// CEP:ASSUMES: @cold (publish path); the bytes are final at registration.
// CEP:COST: @cold. One memcpy + small allocations.
// CEP:EVIDENCE: DoD 19/21/22.
Result<void> GdbJitInterface::register_code(const void* code, size_t size,
                                            const char* symbol) {
    if (code == nullptr || size == 0) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "gdb jit: null or empty code range");
    }
    auto& st = gdb_state();
    std::lock_guard<std::mutex> lock(st.mutex);
    const uint64_t base = reinterpret_cast<uint64_t>(code);
    for (const auto& r : st.live) {
        if (r.code_addr == base) {
            return support::fail(support::ErrorCode::InvalidState,
                                 "gdb jit: duplicate live registration");
        }
    }
    st.live.emplace_back();
    GdbRegistration& reg = st.live.back();
    reg.code_addr = base;
    try {
        reg.symfile = build_gdb_symfile(
            base, static_cast<const uint8_t*>(code), size, symbol);
        reg.entry = std::make_unique<GdbJitCodeEntry>();
    } catch (...) {
        st.live.pop_back();  // nothing was linked into the descriptor
        return support::fail(support::ErrorCode::OutOfMemory,
                             "gdb jit: symfile allocation failed");
    }
    reg.entry->symfile_addr =
        reinterpret_cast<uint64_t>(reg.symfile.data());
    reg.entry->symfile_size = reg.symfile.size();
    // Canonical doubly-linked head insertion (gdb/jit.h).
    reg.entry->next_entry = __jit_debug_descriptor.first_entry;
    reg.entry->prev_entry = nullptr;
    if (reg.entry->next_entry != nullptr) {
        reg.entry->next_entry->prev_entry = reg.entry.get();
    }
    __jit_debug_descriptor.first_entry = reg.entry.get();
    __jit_debug_descriptor.relevant_entry = reg.entry.get();
    __jit_debug_descriptor.action_flag = kJitRegisterFn;
    __jit_debug_register_code();
    return ok();
}

// CEP:WHAT: unregister_code — finds the live base, unlinks the entry from
//           the descriptor chain, signals JIT_UNREGISTER_FN, drops the
//           registration (freeing the symfile snapshot).
// CEP:WHY: The descriptor must never outlive the code (the RAII token on
//          the executable owns the call).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: unknown base / null — named rejects; mutex-serialized.
// CEP:ASSUMES: @cold (executable teardown).
// CEP:COST: @cold. Chain walk + one vector erase.
// CEP:EVIDENCE: DoD 20/21.
Result<void> GdbJitInterface::unregister_code(const void* code) {
    if (code == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "gdb jit: null code range");
    }
    auto& st = gdb_state();
    std::lock_guard<std::mutex> lock(st.mutex);
    const uint64_t base = reinterpret_cast<uint64_t>(code);
    auto it = st.live.begin();
    for (; it != st.live.end(); ++it) {
        if (it->code_addr == base) break;
    }
    if (it == st.live.end()) {
        return support::fail(support::ErrorCode::InvalidState,
                             "gdb jit: unregister of unknown base");
    }
    // Unlink from the doubly-linked chain (gdb/jit.h shape).
    GdbJitCodeEntry* e = it->entry.get();
    if (e->prev_entry != nullptr) {
        e->prev_entry->next_entry = e->next_entry;
    } else {
        __jit_debug_descriptor.first_entry = e->next_entry;
    }
    if (e->next_entry != nullptr) {
        e->next_entry->prev_entry = e->prev_entry;
    }
    // Canonical client behavior: relevant_entry names the removed entry
    // and the action flag says why (gdb reads both at the breakpoint hit —
    // the pointer is consumed by the debugger, never dereferenced later).
    __jit_debug_descriptor.relevant_entry = e;
    __jit_debug_descriptor.action_flag = kJitUnregisterFn;
    __jit_debug_register_code();
    st.live.erase(it);  // relevant_entry now dangles BY CONTRACT (do not
    // "fix" the dangling pointer — canonical client behavior).
    return ok();
}

// CEP:WHAT: The owning-registration helper: register + return a token whose
//           destruction unregisters. A named failure returns null — the
//           publish path never fails for telemetry.
// CEP:WHY: The publish sites hold the token on the executable; teardown
//          order then guarantees the descriptor never outlives the code.
// CEP:STATUS: complete
// CEP:FAILURE: register_code's named rejects surface as a null token.
// CEP:ASSUMES: The captured base stays the executable's identity (the W^X
//          mapping lives exactly as long as the token).
// CEP:COST: @cold.
// CEP:EVIDENCE: the J1/J2 publish sites; DoD 19/20 (token round trip).
std::shared_ptr<void> GdbJitInterface::register_code_owned(
    const void* code, size_t size, const char* symbol) {
    if (!register_code(code, size, symbol).has_value()) return nullptr;
    return std::shared_ptr<void>(const_cast<void*>(code),
                                 [code](void*) {
                                     (void)unregister_code(code);
                                 });
}

// CEP:WHAT: The test-observability reads (live count, membership, symfile
//           span) — mutex-guarded scans over the small live set.
// CEP:WHY: The DoD parse-backs need the descriptor state without poking
//          raw globals from multiple threads.
// CEP:STATUS: complete
// CEP:FAILURE: none (unknown base -> empty span / false).
// CEP:ASSUMES: @cold/test only.
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 19/20/22.
size_t GdbJitInterface::live_entries() {
    auto& st = gdb_state();
    std::lock_guard<std::mutex> lock(st.mutex);
    return st.live.size();
}

bool GdbJitInterface::is_registered(const void* code) {
    if (code == nullptr) return false;
    auto& st = gdb_state();
    std::lock_guard<std::mutex> lock(st.mutex);
    const uint64_t base = reinterpret_cast<uint64_t>(code);
    for (const auto& r : st.live) {
        if (r.code_addr == base) return true;
    }
    return false;
}

std::span<const uint8_t> GdbJitInterface::symfile_of(const void* code) {
    auto& st = gdb_state();
    std::lock_guard<std::mutex> lock(st.mutex);
    const uint64_t base = reinterpret_cast<uint64_t>(code);
    for (const auto& r : st.live) {
        if (r.code_addr == base) return r.symfile;
    }
    return {};
}

// ---- batch e: managed sanitizers -------------------------------------------------

// CEP:WHAT: The policy state (the installed config + the attach latch).
// CEP:WHY: One validated process-wide config; the monotonic latch makes
//          mid-run flips impossible (an untracked assumption change).
// CEP:STATUS: complete
// CEP:FAILURE: none (state, not logic).
// CEP:ASSUMES: Function-local static init.
// CEP:COST: one guard load per accessor read.
// CEP:EVIDENCE: DoD 23/27.
namespace {
struct PolicyState {
    SanitizerPolicy policy;
    bool installed = false;
};
PolicyState& policy_state() noexcept {
    static PolicyState state;
    return state;
}
}  // namespace

// CEP:WHAT: The policy constructor — the reserved guest_tsan bit is a
//           named refusal (never a silent no-op, Rule 120).
// CEP:WHY: docs/infrastructure/06 managed sanitizers section.
// CEP:STATUS: complete
// CEP:FAILURE: guest_tsan -> Unsupported (named).
// CEP:ASSUMES: none.
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 23.
Result<SanitizerPolicy> SanitizerPolicy::make(bool guest_asan,
                                              bool guest_tsan) {
    if (guest_tsan) {
        return support::fail(support::ErrorCode::Unsupported,
                             "sanitizer policy: guest_tsan is reserved "
                             "until the atomics capability pack (post-M6)");
    }
    SanitizerPolicy p;
    p.guest_asan = guest_asan;
    return p;
}

// CEP:WHAT: attach_sanitizer_policy — validates via make(), installs once;
//           a re-attach with a DIFFERENT policy is a named reject.
// CEP:WHY: Flipping sanitizer state under live guest code would invalidate
//          every recorded assumption (the attach is setup-time).
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: the reserved-bit refusal; the mismatch reject.
// CEP:ASSUMES: Called before guest code runs.
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 23/24 (the enabling attach).
Result<void> attach_sanitizer_policy(const SanitizerPolicy& policy) {
    // Validation is the gate (a hand-built policy cannot smuggle the
    // reserved bit past make()'s named refusal).
    auto validated = SanitizerPolicy::make(policy.guest_asan,
                                           policy.guest_tsan);
    if (!validated) return std::unexpected(validated.error());
    PolicyState& st = policy_state();
    if (st.installed &&
        (st.policy.guest_asan != validated->guest_asan ||
         st.policy.guest_tsan != validated->guest_tsan)) {
        return support::fail(support::ErrorCode::InvalidState,
                             "sanitizer policy: re-attach with a different "
                             "policy is refused");
    }
    st.policy = *validated;
    st.installed = true;
    return ok();
}

// CEP:WHAT: The policy accessor.
// CEP:WHY: The consumers' one-branch gate.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Function-local static init.
// CEP:COST: one guard load.
// CEP:EVIDENCE: the T0/FFI consumer sites; DoD 27.
SanitizerPolicy& sanitizer_policy() noexcept { return policy_state().policy; }

// CEP:WHAT: GuestShadow — 1 byte per 8 heap bytes over the heap span
//           (the card table's construction shape); poison/unpoison fill
//           the covered granule range; span_ok checks first+last granules
//           (the standard partial-granule shape).
// CEP:WHY: The GC-corruption detection classes: evacuated-from copies and
//          released-region spans.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — producers are stores; detection is verify_span.
// CEP:ASSUMES: The heap span is fixed at construction; producers are
//          @cold; spans outside the heap are not the shadow's authority
//          (span_ok passes them — foreign buffers stay legal). A span that
//          STARTS in-heap but extends past the end is also passed (the
//          out-of-heap tail is not ours to bless or damn); the FFI/interp
//          callers never construct such spans with live lengths.
// CEP:COST: @cold producers (granule-range fill); span_ok is O(1).
// CEP:EVIDENCE: DoD 24/25/27.
GuestShadow::GuestShadow(const void* heap_base, size_t heap_bytes)
    : base_ptr_(heap_base),
      base_(reinterpret_cast<uint64_t>(heap_base)),
      heap_bytes_(heap_bytes),
      shadow_((heap_bytes >> kShadowShift) + 1, kAddressable) {}

void GuestShadow::poison(uint64_t addr, size_t bytes, uint8_t value) noexcept {
    if (bytes == 0 || addr < base_) return;
    size_t lo = (addr - base_) >> kShadowShift;
    size_t hi = (addr - base_ + bytes - 1) >> kShadowShift;
    if (lo >= shadow_.size()) return;
    if (hi >= shadow_.size()) hi = shadow_.size() - 1;
    for (size_t i = lo; i <= hi; ++i) shadow_[i] = value;
}

void GuestShadow::unpoison(uint64_t addr, size_t bytes) noexcept {
    poison(addr, bytes, kAddressable);
}

bool GuestShadow::span_ok(const void* addr, size_t bytes) const noexcept {
    if (addr == nullptr || bytes == 0) return true;
    const uint64_t a = reinterpret_cast<uint64_t>(addr);
    if (a < base_ || a - base_ + bytes > heap_bytes_) return true;
    const size_t lo = (a - base_) >> kShadowShift;
    const size_t hi = (a - base_ + bytes - 1) >> kShadowShift;
    return shadow_[lo] == kAddressable && shadow_[hi] == kAddressable;
}

uint8_t GuestShadow::poison_kind(const void* addr) const noexcept {
    if (addr == nullptr) return kAddressable;
    const uint64_t a = reinterpret_cast<uint64_t>(addr);
    if (a < base_ || a - base_ >= heap_bytes_) return kAddressable;
    return shadow_[(a - base_) >> kShadowShift];
}

namespace {
GuestShadow* g_guest_shadow = nullptr;  // the Heap attaches; it outlives
}  // namespace

// CEP:WHAT: The shadow attach/accessors (raw, no ownership — the heap
//           outlives every guest execution and detaches at destruction).
// CEP:WHY: verify_span routes through the CURRENT heap's shadow (the
//          gc_code_registry accessor discipline).
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: One live engine heap at a time (Rule 83 world).
// CEP:COST: one guard load.
// CEP:EVIDENCE: the Heap producers; DoD 24-26.
GuestShadow* guest_shadow() noexcept { return g_guest_shadow; }

void attach_guest_shadow(GuestShadow* shadow) noexcept {
    g_guest_shadow = shadow;
}

// CEP:WHAT: verify_span — the ONE check every consumer routes through.
//           Disabled policy or no shadow: always ok (the cost contract).
//           A poisoned span: the named GuestSanitizer failure carrying the
//           address, a SanitizerTrap replay event, and NO abort.
// CEP:WHY: One named failure shape for T0 ops, the FFI boundary, and the
//          tests (docs/infrastructure/06 managed sanitizers section).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: GuestSanitizer (named, kind+address in the message).
// CEP:ASSUMES: noexcept — the diagnostic build is allocation-wrapped and
//          degrades to a static message on OOM.
// CEP:COST: disabled — one relaxed bool load; enabled — O(1) granule pair.
// CEP:EVIDENCE: DoD 24-27.
Result<void> verify_span(const void* addr, size_t bytes) noexcept {
    if (!sanitizer_policy().guest_asan) return ok();
    GuestShadow* sh = guest_shadow();
    if (sh == nullptr || sh->span_ok(addr, bytes)) return ok();
    const uint8_t kind = sh->poison_kind(addr);
    replay_recorder().record_event(
        static_cast<uint32_t>(ReplayKind::SanitizerTrap),
        reinterpret_cast<uint64_t>(addr) & 0xFFFFFFFFull,
        static_cast<uint64_t>(kind));
    // noexcept discipline: every allocation-bearing step (the message
    // build, the Diagnostic's string) lives inside the guard; the OOM
    // fallback is an SSO-bound literal that cannot allocate.
    try {
        char msg[112];
        const int n = ::snprintf(msg, sizeof(msg),
                                 "guest sanitizer: poisoned span (kind=%u) "
                                 "at 0x%llx",
                                 static_cast<unsigned>(kind),
                                 static_cast<unsigned long long>(
                                     reinterpret_cast<uint64_t>(addr)));
        if (n > 0) {
            return support::fail(support::ErrorCode::GuestSanitizer,
                                 std::string(msg, static_cast<size_t>(n)));
        }
    } catch (...) {
    }
    return support::fail(support::ErrorCode::GuestSanitizer,
                         std::string("sanitizer"));
}

// ---- batch e: deterministic replay ----------------------------------------------

namespace {
// CEP:WHAT: The flag's storage (shared by the accessor pair).
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Flipped before the run under test.
// CEP:COST: one relaxed bool load.
// CEP:EVIDENCE: DoD 31.
bool& replay_flag() noexcept {
    static bool flag = false;
    return flag;
}
}  // namespace

// CEP:WHAT: The replay flag (the ONE switch every producer reads).
// CEP:WHY: The zero-cost-off contract stays auditable through one bool.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Flipped before the run under test.
// CEP:COST: one relaxed bool load.
// CEP:EVIDENCE: DoD 31.
bool replay_enabled() noexcept { return replay_flag(); }

void set_replay_enabled(bool enabled) noexcept { replay_flag() = enabled; }

// CEP:WHAT: record_event — no-op under a disabled flag; under the flag:
//           monotonic seq, ring push, Rule-28 clear-on-overflow with the
//           seq/total surviving the clear.
// CEP:WHY: The decision stream the differential checker compares.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Single producer per site (the driver/mutator discipline).
// CEP:COST: disabled — one bool load; enabled — one push.
// CEP:EVIDENCE: DoD 28-31.
void ReplayRecorder::record_event(uint32_t kind, uint64_t a,
                                  uint64_t b) noexcept {
    if (!replay_enabled()) return;
    if (events_.size() >= kMaxEvents) events_.clear();
    try {
        events_.push_back(ReplayEvent{seq_ + 1, kind, a, b});
    } catch (...) {
        // Degradation: the event is dropped and NOT counted (Rule 120 —
        // counters never claim an event that was never stored).
        return;
    }
    ++seq_;
    ++total_events_;
}

// CEP:WHAT: Clears the ring (tests + the differential harness between
//           runs); seq/total survive by design.
// CEP:WHY: The harness reuses the process singleton across runs.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Called between runs (not concurrent with producers).
// CEP:COST: @cold. O(1) vector clear.
// CEP:EVIDENCE: DoD 28/30.
void ReplayRecorder::clear() noexcept { events_.clear(); }

// CEP:WHAT: The process recorder singleton.
// CEP:WHY: Tiering/T0/GC/RBPD/publish/sanitizer producers share one stream.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: Function-local static init.
// CEP:COST: one guard load.
// CEP:EVIDENCE: the producer sites.
ReplayRecorder& replay_recorder() noexcept {
    static ReplayRecorder recorder;
    return recorder;
}

// CEP:WHAT: compare_streams — the O(n) first-divergence walk: kind or
//           payload mismatch stops the scan and reports both sides; a
//           length mismatch diverges at the shared length.
// CEP:WHY: The differential harness's output — the first divergence IS
//          the diagnosis.
// CEP:STATUS: complete
// CEP:FAILURE: none (pure comparison).
// CEP:ASSUMES: none.
// CEP:COST: @cold. O(n).
// CEP:EVIDENCE: DoD 28/29.
ReplayDivergence compare_streams(const ReplayRecorder& expected,
                                 const ReplayRecorder& actual) noexcept {
    ReplayDivergence d;
    const auto& ev = expected.events();
    const auto& av = actual.events();
    const size_t n = ev.size() < av.size() ? ev.size() : av.size();
    for (size_t i = 0; i < n; ++i) {
        if (ev[i].kind != av[i].kind || ev[i].a != av[i].a ||
            ev[i].b != av[i].b) {
            d.identical = false;
            d.first_divergence = i;
            d.kind = ev[i].kind;
            d.expected_a = ev[i].a;
            d.expected_b = ev[i].b;
            d.actual_a = av[i].a;
            d.actual_b = av[i].b;
            return d;
        }
    }
    if (ev.size() != av.size()) {
        d.identical = false;
        d.first_divergence = n;
        if (n < ev.size()) {
            d.kind = ev[n].kind;
            d.expected_a = ev[n].a;
            d.expected_b = ev[n].b;
        } else {
            d.kind = av[n].kind;
            d.actual_a = av[n].a;
            d.actual_b = av[n].b;
        }
    }
    return d;
}

// ---- batch e: the named+line record_code overload ------------------------------

// CEP:WHAT: The named+line publish hook: the real (jitdump-raw) symbol and
//           the source-line debug entries through to CODE_LOAD; the perf
//           map gets the sanitized name. Empty name -> the synthetic form
//           is the CALLER's choice (overload 1) — this overload requires
//           a name.
// CEP:WHY: The metadata interchange upgrade (docs/infrastructure/06 —
//          real symbol names and jitdump debug entries).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: never fails the caller (last_error() stores the degradation).
// CEP:ASSUMES: Driver thread; final code bytes (publish discipline).
// CEP:COST: @cold. One active() branch when idle.
// CEP:EVIDENCE: DoD 32/33.
void JitSession::record_code(const void* code_base, size_t code_bytes,
                             const std::string& name,
                             std::span<const JitDumpWriter::DebugEntry>
                                 debug) noexcept {
    if (!active_) return;
    try {
        const uint64_t base = reinterpret_cast<uint64_t>(code_base);
        if (auto r = jitdump_.record_code_load(base, base, code_bytes,
                                               code_index_, name, debug);
            !r) {
            last_error_ = r.error().format();
            return;
        }
        ++code_index_;
        std::string sanitized;
        sanitized.reserve(name.size());
        for (const char c : name) {
            sanitized.push_back(
                (c == ' ' || c == '\n') ? '_' : c);
        }
        if (auto r = perf_map_.record(base, code_bytes, sanitized); !r) {
            last_error_ = r.error().format();
            return;
        }
        ++code_loads_;  // COMPLETE publications only (see the overload above)
    } catch (const std::exception& e) {
        try {
            last_error_ = std::string("observability: record_code: ") +
                          e.what();
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

}  // namespace vortex::infra
