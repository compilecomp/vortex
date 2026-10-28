// CEP:FILE: tests/test_observability.cpp
// CEP:WHAT: Infra 6 observability test suite (M6-d floor DoD 1-16 + batch
//           e DoD 18-33, docs/infrastructure/06-observability.md): the perf-jitdump
//           v1 header/record contract (parse-back, monotonic timestamps,
//           CODE_CLOSE completeness, self-mmap probe, named rejects), the
//           perf-map sidecar format + sanitization, the publish-path
//           CODE_LOAD records (J1 + J2) and the telemetry-failure publish
//           guarantee, the handshake TTSP ring (freeze ttsp/body/generation,
//           single-thread handshake, timed-out rollback, bounded ring), the
//           GC pause-ring cause labels through a REAL engine cycle (occupancy
//           trigger) and a REAL young collection (allocation slow path), and
//           the allocation sampler's interval/ring/counter contract.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry — every byte layout and every counter needs a
//           pin that fails loudly when the contract drifts (the M1
//           golden-pin precedent, applied to telemetry).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg; a parse failure of the
//           writer's own output fails the test (no silent pass).
// CEP:ASSUMES: Linux x86-64 (/proc/self/maps for the self-mmap probe,
//           CLOCK_MONOTONIC); /tmp is writable for temp files (cleaned up);
//           the J1/J2 publish paths accept minimal hand-built code structs
//           (the hooks are what's under test, not the compilers).
// CEP:COST: @cold test process.
// CEP:EVIDENCE: this file (each test names its DoD item).
#include "vortex_test.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "vortex/gc/collector.hpp"
#include "vortex/gc/icggc.hpp"
#include "vortex/infra/code_range.hpp"
#include "vortex/infra/ffi.hpp"  // infra::HandleTable (the fixture wires it)
#include "vortex/infra/observability.hpp"
#include "vortex/infra/threading.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j1/stencil_corpus.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/verifier.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/runtime/object_model.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;
using namespace vortex::infra;

namespace {

int g_temp_counter = 0;

// CEP:WHAT: Unique temp-file path per call (per-pid + counter); tests unlink
//           their files at the end.
// CEP:WHY: Tests must never share file state (the fixture rule).
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: /tmp is writable.
// CEP:COST: @cold.
// CEP:EVIDENCE: every file-backed test in this file.
std::string temp_path(const char* suffix) {
    return "/tmp/vx_obs_test_" + std::to_string(::getpid()) + "_" +
           std::to_string(g_temp_counter++) + suffix;
}

// ---- jitdump parse-back helpers ------------------------------------------------

struct ParsedRecord {
    uint32_t id = 0;
    uint32_t size = 0;
    uint64_t ts = 0;
    std::vector<uint8_t> payload;  // after the 16-byte framing header
};

struct ParsedJitdump {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t total_size = 0;
    uint32_t elf_mach = 0;
    uint32_t pad1 = 0;
    uint32_t pid = 0;
    uint64_t timestamp = 0;
    uint64_t flags = 0;
    std::vector<ParsedRecord> records;
};

// CEP:WHAT: Reads one little-endian u32 from a parsed byte buffer.
// CEP:WHY: The parse-back helpers read the writer's output as perf would.
// CEP:STATUS: complete
// CEP:FAILURE: none (caller bounds the offset).
// CEP:ASSUMES: off + 4 <= buffer size.
// CEP:COST: @cold.
// CEP:EVIDENCE: every jitdump DoD test.
uint32_t le_u32(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) |
           (static_cast<uint32_t>(b[off + 3]) << 24);
}

// CEP:WHAT: Reads one little-endian u64 from a parsed byte buffer.
// CEP:WHY: Same reader contract as le_u32 at twice the width.
// CEP:STATUS: complete
// CEP:FAILURE: none (caller bounds the offset).
// CEP:ASSUMES: off + 8 <= buffer size.
// CEP:COST: @cold.
// CEP:EVIDENCE: every jitdump DoD test.
uint64_t le_u64(const std::vector<uint8_t>& b, size_t off) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[off + i];
    return v;
}

// CEP:WHAT: Parses a jitdump file byte-exactly per the v1 spec (40-byte
//           header, then record framing walks).
// CEP:WHY: The DoD parse-back tests read the WRITER's output as perf would.
// CEP:STATUS: complete
// CEP:FAILURE: returns false on any malformed structure (the test fails).
// CEP:ASSUMES: the file is complete (the writer closed it).
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 1-4, 9.
bool parse_jitdump(const std::string& path, ParsedJitdump& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::vector<uint8_t> bytes;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        bytes.insert(bytes.end(), buf, buf + n);
    }
    std::fclose(f);
    if (bytes.size() < 40) return false;
    out.magic = le_u32(bytes, 0);
    out.version = le_u32(bytes, 4);
    out.total_size = le_u32(bytes, 8);
    out.elf_mach = le_u32(bytes, 12);
    out.pad1 = le_u32(bytes, 16);
    out.pid = le_u32(bytes, 20);
    out.timestamp = le_u64(bytes, 24);
    out.flags = le_u64(bytes, 32);
    size_t off = out.total_size;
    while (off + 16 <= bytes.size()) {
        ParsedRecord rec;
        rec.id = le_u32(bytes, off);
        rec.size = le_u32(bytes, off + 4);
        rec.ts = le_u64(bytes, off + 8);
        if (rec.size < 16 || off + rec.size > bytes.size()) return false;
        rec.payload.assign(bytes.begin() + static_cast<long>(off + 16),
                           bytes.begin() + static_cast<long>(off + rec.size));
        out.records.push_back(std::move(rec));
        off += rec.size;
    }
    return off == bytes.size();
}

// CEP:WHAT: Reads the perf-map sidecar into lines.
// CEP:WHY: DoD 8/9 assert the line format and the record pairing.
// CEP:STATUS: complete
// CEP:FAILURE: returns false when the file is unreadable.
// CEP:ASSUMES: one line per code load.
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 8/9.
bool read_lines(const std::string& path, std::vector<std::string>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::string all;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        all.append(reinterpret_cast<const char*>(buf), n);
    }
    std::fclose(f);
    size_t start = 0;
    while (start < all.size()) {
        const size_t nl = all.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(all.substr(start));
            break;
        }
        out.push_back(all.substr(start, nl - start));
        start = nl + 1;
    }
    return true;
}

// CEP:WHAT: Removes a test temp file, ignoring absence.
// CEP:WHY: Tests clean up their /tmp artifacts even on failure paths.
// CEP:STATUS: complete
// CEP:FAILURE: none (best-effort cleanup).
// CEP:ASSUMES: the path belongs to this test run.
// CEP:COST: @cold.
// CEP:EVIDENCE: every file-backed test's last line.
void unlink_quiet(const std::string& path) { ::unlink(path.c_str()); }

// ---- GC fixture (the test_gc shape, compact) -----------------------------------

constexpr size_t kYoungBytes = 256 * 1024;
constexpr size_t kOldBytes = 2 * 1024 * 1024;

struct Fixture {
    gc::Heap heap{kYoungBytes, kOldBytes};
    vm::Interpreter interp{heap};
    infra::HandleTable handles;
    // CEP:WHAT: Per-test fixture: small heap + interpreter + handle table.
    // CEP:WHY: tests must never share heap state.
    // CEP:STATUS: complete
    // CEP:FAILURE: none.
    // CEP:ASSUMES: the heap outlives the interpreter in member order.
    // CEP:COST: @cold.
    // CEP:EVIDENCE: the engine-cycle and young-GC cause tests.
    Fixture() {
        heap.gc_state().handles = &handles;
        heap.gc_state().interpreter = &interp;
    }
};

// CEP:WHAT: Creates a C++-owned klass descriptor with named fields (the
//           test_gc shape).
// CEP:WHY: Klass descriptors live outside the traced heap (M0 rule); tests
//           need named fields for allocation fixtures.
// CEP:STATUS: complete
// CEP:FAILURE: none (host allocation).
// CEP:ASSUMES: the static registry keeps them alive for the process.
// CEP:COST: @cold.
// CEP:EVIDENCE: the engine-cycle and young-GC cause tests.
Klass* make_klass(Fixture& f, const char* name, uint32_t fields) {
    struct Entry {
        std::unique_ptr<Klass> k;
    };
    static std::vector<Entry> registry;
    registry.push_back(Entry{std::make_unique<Klass>(name)});
    Klass* k = registry.back().k.get();
    for (uint32_t i = 0; i < fields; ++i) {
        k->add_field("f" + std::to_string(i));
    }
    (void)f;
    return k;
}

// CEP:WHAT: Places a young object's copy directly in old space (the
//           occupancy-fill shape the evacuator produces).
// CEP:WHY: The OccupancyTrigger cause test needs old-space pressure without
//           a real young GC.
// CEP:STATUS: complete
// CEP:FAILURE: returns nullptr when old space exhausts.
// CEP:ASSUMES: the young header is valid; the copy self-forwards.
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 15.
HeapObject* fill_old_copy(gc::Heap& heap, Object* young) {
    const size_t bytes = young->header.size;
    auto* raw = heap.promote(bytes);
    if (raw == nullptr) return nullptr;
    std::memcpy(raw, young, bytes);
    raw->header.brooks = raw;
    raw->header.mark = 0;
    return raw;
}

// CEP:WHAT: Probes /proc/self/maps for a file-backed mapping of `path` with
//           the execute bit and a page-aligned start.
// CEP:WHY: DoD 7 — the jitdump self-mmap requirement (perf's discovery
//           beacon).
// CEP:STATUS: complete
// CEP:FAILURE: returns false when no matching mapping exists.
// CEP:ASSUMES: Linux /proc layout.
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 7.
bool probe_exec_mapping(const std::string& path) {
    std::FILE* f = std::fopen("/proc/self/maps", "rb");
    if (f == nullptr) return false;
    char line[1024];
    bool found = false;
    const long page = ::sysconf(_SC_PAGESIZE);
    while (!found && std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strstr(line, path.c_str()) == nullptr) continue;
        // "start-end perms offset dev inode path"
        unsigned long long start = 0;
        char perms[8] = {0};
        if (std::sscanf(line, "%llx-%*llx %7s", &start, perms) >= 2) {
            if (std::strchr(perms, 'x') != nullptr && page > 0 &&
                (start & static_cast<unsigned long long>(page - 1)) == 0) {
                found = true;
            }
        }
    }
    std::fclose(f);
    return found;
}

}  // namespace

// ---- DoD 1: the header -----------------------------------------------------------

// CEP:WHAT: Test: DoD 1 - the 40-byte v1 header (magic/version/total_size/elf_mach/pad/pid/timestamp/flags) parse back byte-exactly, and the destructor's close lands a complete CODE_CLOSE record.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(jitdump_header_matches_format) {
    const std::string path = temp_path(".jitdump");
    {
        JitDumpWriter w;
        auto opened = w.open(path);
        VORTEX_EXPECT(opened.has_value());
        VORTEX_EXPECT(w.is_open());
    }  // destructor closes (best-effort)
    ParsedJitdump p;
    VORTEX_EXPECT(parse_jitdump(path, p));
    VORTEX_EXPECT_EQ(p.magic, 0x4A695444u);
    VORTEX_EXPECT_EQ(p.version, 1u);
    VORTEX_EXPECT_EQ(p.total_size, 40u);
    VORTEX_EXPECT_EQ(p.elf_mach, 62u);
    VORTEX_EXPECT_EQ(p.pad1, 0u);
    VORTEX_EXPECT_EQ(p.pid, static_cast<uint32_t>(::getpid()));
    VORTEX_EXPECT(p.timestamp > 0);
    VORTEX_EXPECT_EQ(p.flags, 0ull);
    // The destructor's close() writes CODE_CLOSE — the parse-back proves the
    // destructor path lands a complete, well-formed file.
    VORTEX_EXPECT_EQ(p.records.size(), 1u);
    VORTEX_EXPECT_EQ(p.records[0].id, 3u);
    VORTEX_EXPECT_EQ(p.records[0].size, 16u);
    unlink_quiet(path);
}

// ---- DoD 2: CODE_LOAD roundtrip ---------------------------------------------------

// CEP:WHAT: Test: DoD 2 - one CODE_LOAD record parses back whole: framing, pid/tid, vma/code_addr/code_size/code_index, exact name bytes + NUL, nr_debug_entries 0.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(jitdump_code_load_record_roundtrip) {
    const std::string path = temp_path(".jitdump");
    {
        JitDumpWriter w;
        VORTEX_EXPECT(w.open(path).has_value());
        auto rec = w.record_code_load(0x1000, 0x1000, 16, 0, "vx_j1_m7");
        VORTEX_EXPECT(rec.has_value());
        VORTEX_EXPECT(w.close().has_value());
        VORTEX_EXPECT(!w.is_open());
    }
    ParsedJitdump p;
    VORTEX_EXPECT(parse_jitdump(path, p));
    // One CODE_LOAD + the destructor's CODE_CLOSE.
    VORTEX_EXPECT_EQ(p.records.size(), 2u);
    const ParsedRecord& r = p.records[0];
    VORTEX_EXPECT_EQ(r.id, 0u);
    // size = framing(16) + pid/tid(8) + code fields(32) + name(9 incl NUL)
    //        + nr_debug_entries(4)
    VORTEX_EXPECT_EQ(r.size, 16u + 8u + 32u + 9u + 4u);
    VORTEX_EXPECT(r.ts >= p.timestamp);
    VORTEX_EXPECT_EQ(le_u32(r.payload, 0), static_cast<uint32_t>(::getpid()));
    // tid > 0 (gettid)
    VORTEX_EXPECT(le_u32(r.payload, 4) > 0);
    VORTEX_EXPECT_EQ(le_u64(r.payload, 8), 0x1000ull);   // vma
    VORTEX_EXPECT_EQ(le_u64(r.payload, 16), 0x1000ull);  // code_addr
    VORTEX_EXPECT_EQ(le_u64(r.payload, 24), 16ull);      // code_size
    VORTEX_EXPECT_EQ(le_u64(r.payload, 32), 0ull);       // code_index
    // name at 40: "vx_j1_m7\0"
    VORTEX_EXPECT(r.payload.size() >= 49);
    VORTEX_EXPECT(std::memcmp(r.payload.data() + 40, "vx_j1_m7\0", 9) == 0);
    // nr_debug_entries = 0 (reserved field, written zero-filled)
    VORTEX_EXPECT_EQ(le_u32(r.payload, 49), 0u);
    unlink_quiet(path);
}

// ---- DoD 3: monotonic timestamps ---------------------------------------------------

// CEP:WHAT: Test: DoD 3 - a five-record sequence's CLOCK_MONOTONIC timestamps are non-decreasing.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(jitdump_timestamps_monotonic) {
    const std::string path = temp_path(".jitdump");
    {
        JitDumpWriter w;
        VORTEX_EXPECT(w.open(path).has_value());
        for (int i = 0; i < 5; ++i) {
            VORTEX_EXPECT(
                w.record_code_load(0x2000, 0x2000, 8, static_cast<uint64_t>(i),
                                   "vx_j1_m" + std::to_string(i))
                    .has_value());
        }
        VORTEX_EXPECT(w.close().has_value());
    }
    ParsedJitdump p;
    VORTEX_EXPECT(parse_jitdump(path, p));
    VORTEX_EXPECT_EQ(p.records.size(), 6u);  // 5 loads + the close record
    for (size_t i = 1; i < p.records.size(); ++i) {
        VORTEX_EXPECT(p.records[i].ts >= p.records[i - 1].ts);
    }
    unlink_quiet(path);
}

// ---- DoD 4: session stop writes CODE_CLOSE ----------------------------------------

// CEP:WHAT: Test: DoD 4 - JitSession::stop writes JIT_CODE_CLOSE framing and closes; the file parses back completely.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(jitdump_close_record_and_stop) {
    const std::string jit = temp_path(".jitdump");
    const std::string map = temp_path(".map");
    {
        JitSession& s = jit_session();
        VORTEX_EXPECT(s.start(jit, map).has_value());
        // record_code is best-effort void (never fails the caller).
        s.record_code(reinterpret_cast<const void*>(0x3000), 32, 11, "j1");
        VORTEX_EXPECT(s.stop().has_value());
        VORTEX_EXPECT(!s.active());
    }
    ParsedJitdump p;
    VORTEX_EXPECT(parse_jitdump(jit, p));
    VORTEX_EXPECT_EQ(p.records.size(), 2u);
    VORTEX_EXPECT_EQ(p.records[0].id, 0u);
    VORTEX_EXPECT_EQ(p.records[1].id, 3u);  // JIT_CODE_CLOSE
    VORTEX_EXPECT_EQ(p.records[1].size, 16u);
    unlink_quiet(jit);
    unlink_quiet(map);
}

// ---- DoD 5: double open rejects; restart succeeds ----------------------------------

// CEP:WHAT: Test: DoD 5 - a second start while a session is active rejects by name; after stop, a restart succeeds (the one-producer rule is per-time).
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(jitdump_double_open_rejects_named) {
    const std::string jit = temp_path(".jitdump");
    const std::string map = temp_path(".map");
    JitSession& s = jit_session();
    VORTEX_EXPECT(s.start(jit, map).has_value());
    auto again = s.start(jit + "2", map + "2");
    VORTEX_EXPECT(!again.has_value());
    VORTEX_EXPECT(again.error().message.find("already active") !=
                  std::string::npos);
    VORTEX_EXPECT(s.stop().has_value());
    // After a stop, a restart succeeds (the one-producer rule is per-time).
    VORTEX_EXPECT(s.start(jit + "3", map + "3").has_value());
    VORTEX_EXPECT(s.stop().has_value());
    unlink_quiet(jit);
    unlink_quiet(map);
    unlink_quiet(jit + "2");
    unlink_quiet(map + "2");
    unlink_quiet(jit + "3");
    unlink_quiet(map + "3");
}

// ---- DoD 6: record-after-stop rejects ----------------------------------------------

// CEP:WHAT: Test: DoD 6 - record/close on a closed writer are named rejects with the documented message shape.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(jitdump_record_after_stop_rejects_named) {
    const std::string path = temp_path(".jitdump");
    JitDumpWriter w;
    VORTEX_EXPECT(w.open(path).has_value());
    VORTEX_EXPECT(w.close().has_value());
    auto rec = w.record_code_load(0x4000, 0x4000, 8, 0, "late");
    VORTEX_EXPECT(!rec.has_value());
    VORTEX_EXPECT(rec.error().message.find("closed") != std::string::npos);
    auto again = w.close();
    VORTEX_EXPECT(!again.has_value());
    unlink_quiet(path);
}

// ---- DoD 7: the self-mmap probe -----------------------------------------------------

// CEP:WHAT: Test: DoD 7 - the session's self-mmap of the jitdump file exists, is page-aligned, and carries the execute bit (the perf discovery beacon).
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(jitdump_self_mmap_is_exec_and_page_aligned) {
    const std::string jit = temp_path(".jitdump");
    const std::string map = temp_path(".map");
    JitSession& s = jit_session();
    VORTEX_EXPECT(s.start(jit, map).has_value());
    VORTEX_EXPECT(probe_exec_mapping(jit));
    VORTEX_EXPECT(s.stop().has_value());
    unlink_quiet(jit);
    unlink_quiet(map);
}

// ---- DoD 8: perf-map format + sanitization ------------------------------------------

// CEP:WHAT: Test: DoD 8 - perf-map lines are '<hex_start> <hex_size> <name>' with no 0x prefixes and whitespace-sanitized names.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(perfmap_line_format_and_sanitization) {
    const std::string path = temp_path(".map");
    {
        PerfMap m;
        VORTEX_EXPECT(m.open(path).has_value());
        VORTEX_EXPECT(m.record(0x1234, 0x20, "name with space").has_value());
        VORTEX_EXPECT(m.record(0x1300, 0x10, "tab\tname").has_value());
        VORTEX_EXPECT(m.close().has_value());
    }
    std::vector<std::string> lines;
    VORTEX_EXPECT(read_lines(path, lines));
    VORTEX_EXPECT_EQ(lines.size(), 2u);
    VORTEX_EXPECT_EQ(lines[0], std::string("1234 20 name_with_space"));
    VORTEX_EXPECT_EQ(lines[1], std::string("1300 10 tab_name"));
    unlink_quiet(path);
}

// ---- DoD 9: publish-path records -----------------------------------------------------

// CEP:WHAT: Test: DoD 9 - with a session open, publish_baseline + publish_j2 append CODE_LOAD records matching the executables' memory ranges and two perf-map lines; code_loads counts 2.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(publish_path_records_jit_code_loads) {
    const std::string jit = temp_path(".jitdump");
    const std::string map = temp_path(".map");
    JitSession& s = jit_session();
    VORTEX_EXPECT(s.start(jit, map).has_value());

    j1::BaselineCode bc;
    bc.method_id = 5;
    bc.code = {0xC3};  // ret — the compilers are not under test; the hooks are
    bc.osr_entry_offset = 0xFFFFFFFFu;
    auto ex1 = j1::publish_baseline(bc, nullptr);
    VORTEX_EXPECT(ex1.has_value());

    j2::J2Code jc;
    jc.method_id = 6;
    jc.code = {0xC3};
    jc.entry_offset = 0;
    auto ex2 = j2::publish_j2(jc, nullptr);
    VORTEX_EXPECT(ex2.has_value());

    VORTEX_EXPECT(s.stop().has_value());
    VORTEX_EXPECT_EQ(s.code_loads(), 2u);

    ParsedJitdump p;
    VORTEX_EXPECT(parse_jitdump(jit, p));
    VORTEX_EXPECT_EQ(p.records.size(), 3u);  // 2 loads + close
    const uint64_t base1 = reinterpret_cast<uint64_t>(ex1->memory.data());
    const uint64_t base2 = reinterpret_cast<uint64_t>(ex2->memory.data());
    VORTEX_EXPECT_EQ(le_u64(p.records[0].payload, 16), base1);
    VORTEX_EXPECT_EQ(le_u64(p.records[0].payload, 24),
                     static_cast<uint64_t>(ex1->memory.size()));
    VORTEX_EXPECT_EQ(le_u64(p.records[1].payload, 16), base2);
    VORTEX_EXPECT_EQ(le_u64(p.records[1].payload, 24),
                     static_cast<uint64_t>(ex2->memory.size()));

    std::vector<std::string> lines;
    VORTEX_EXPECT(read_lines(map, lines));
    VORTEX_EXPECT_EQ(lines.size(), 2u);
    // The sidecar lines must carry the SAME ranges as the jitdump records.
    {
        char expect0[96];
        char expect1[96];
        std::snprintf(expect0, sizeof(expect0), "%zx %zx vx_j1_m5",
                      base1, static_cast<uint64_t>(ex1->memory.size()));
        std::snprintf(expect1, sizeof(expect1), "%zx %zx vx_j2_m6",
                      base2, static_cast<uint64_t>(ex2->memory.size()));
        VORTEX_EXPECT_EQ(lines[0], std::string(expect0));
        VORTEX_EXPECT_EQ(lines[1], std::string(expect1));
    }
    unlink_quiet(jit);
    unlink_quiet(map);
}

// ---- DoD 10: telemetry failure never fails a publish ---------------------------------

// CEP:WHAT: Test: DoD 10 - an uncreatable telemetry target rejects the START by name and never the publish; stop on an inactive session is a named reject.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(publish_survives_telemetry_failure) {
    JitSession& s = jit_session();
    // An uncreatable telemetry target: the start rejects by name...
    auto started = s.start("/proc/vx_obs_no_such_dir/x.jitdump",
                           "/proc/vx_obs_no_such_dir/x.map");
    VORTEX_EXPECT(!started.has_value());
    VORTEX_EXPECT(!s.active());
    // ...and the publish path is untouched (the hook is a no-op when
    // inactive — the failure never rejects correctly-compiled code).
    j1::BaselineCode bc;
    bc.method_id = 7;
    bc.code = {0xC3};
    bc.osr_entry_offset = 0xFFFFFFFFu;
    auto ex = j1::publish_baseline(bc, nullptr);
    VORTEX_EXPECT(ex.has_value());
    // stop on an inactive session is a named reject
    auto stopped = s.stop();
    VORTEX_EXPECT(!stopped.has_value());
}

// CEP:WHAT: Test: DoD 10's record-time half — a telemetry side that opens
//           but cannot WRITE (/dev/full, ENOSPC on the first write) must
//           store the named error in last_error() while the publish still
//           succeeds and code_loads stays honest.
// CEP:WHY: The never-fails-the-publish contract extends past the open path
//           to the write path (the full-disk class).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: /dev/full exists (Linux); the jitdump side uses a real file
//           so the CODE_LOAD itself succeeds and the failure isolates to
//           the perf-map write.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test (DoD 10, second half).
VORTEX_TEST(publish_telemetry_write_failure_stores_named_error) {
    const std::string jit = temp_path(".jitdump");
    JitSession& s = jit_session();
    // The jitdump side is a real file (records succeed); the perf-map side
    // is /dev/full — the open succeeds, the first write fails ENOSPC.
    VORTEX_EXPECT(s.start(jit, "/dev/full").has_value());
    j1::BaselineCode bc;
    bc.method_id = 9;
    bc.code = {0xC3};
    bc.osr_entry_offset = 0xFFFFFFFFu;
    auto ex = j1::publish_baseline(bc, nullptr);
    VORTEX_EXPECT(ex.has_value());  // the publish NEVER fails
    VORTEX_EXPECT(!s.last_error().empty());  // the stored named error
    VORTEX_EXPECT(s.last_error().find("perf-map") != std::string::npos);
    // code_loads counts COMPLETE records (jitdump + perf-map) — the partial
    // failure is not counted (Rule 120 honesty).
    VORTEX_EXPECT_EQ(s.code_loads(), 0ull);
    VORTEX_EXPECT(s.stop().has_value());
    unlink_quiet(jit);
}

// ---- DoD 11: freeze TTSP + body -------------------------------------------------------

// CEP:WHAT: Test: DoD 11 - freeze_all over the (empty) alive set + thaw records one Freeze event: ttsp > 0, body > 0, threads == alive set, generation matches; a second cycle chains generations.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(handshake_freeze_records_ttsp_and_body) {
    HandshakeManager hm;
    SafepointLog log;
    hm.set_safepoint_log(&log);
    // Zero registered threads: the freeze covers the arm + request window
    // (the alive set is empty — threads == 0 is the deterministic shape).
    auto gen = hm.freeze_all(SuspensionKind::Gc);
    VORTEX_EXPECT(gen.has_value());
    VORTEX_EXPECT_EQ(*gen, 1ull);
    VORTEX_EXPECT_EQ(log.events().size(), 0u);  // the event completes at thaw
    VORTEX_EXPECT(hm.thaw_all().has_value());
    VORTEX_EXPECT_EQ(log.events().size(), 1u);
    const SafepointEvent& e = log.events()[0];
    VORTEX_EXPECT(e.kind == SafepointEvent::Kind::Freeze);
    VORTEX_EXPECT_EQ(e.cause, static_cast<uint8_t>(SuspensionKind::Gc));
    VORTEX_EXPECT(e.ttsp_ns > 0);
    VORTEX_EXPECT(e.body_ns > 0);
    VORTEX_EXPECT_EQ(e.threads, 0u);
    VORTEX_EXPECT(!e.timed_out);
    VORTEX_EXPECT_EQ(e.generation, 1ull);
    // A second freeze/thaw: generation 2, two events.
    auto gen2 = hm.freeze_all(SuspensionKind::Profiling);
    VORTEX_EXPECT(gen2.has_value());
    VORTEX_EXPECT_EQ(*gen2, 2ull);
    VORTEX_EXPECT(hm.thaw_all().has_value());
    VORTEX_EXPECT_EQ(log.events().size(), 2u);
    VORTEX_EXPECT_EQ(log.events()[1].generation, 2ull);
    VORTEX_EXPECT_EQ(log.events()[1].cause,
                     static_cast<uint8_t>(SuspensionKind::Profiling));
    hm.set_safepoint_log(nullptr);
}

// ---- DoD 12: single-thread handshake event ---------------------------------------------

// CEP:WHAT: Test: DoD 12 - a request/acknowledge/wait round trip records one Handshake event with ttsp > 0, threads 1, and the request's kind as cause.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(handshake_single_thread_records_handshake_event) {
    HandshakeManager hm;
    SafepointLog log;
    hm.set_safepoint_log(&log);
    auto tid = hm.register_thread();
    VORTEX_EXPECT(tid.has_value());
    VORTEX_EXPECT(hm.request(SuspensionKind::Deopt, *tid).has_value());
    VORTEX_EXPECT(hm.acknowledge(*tid).has_value());
    VORTEX_EXPECT(hm.wait_acknowledged(*tid).has_value());
    VORTEX_EXPECT_EQ(log.events().size(), 1u);
    const SafepointEvent& e = log.events()[0];
    VORTEX_EXPECT(e.kind == SafepointEvent::Kind::Handshake);
    VORTEX_EXPECT_EQ(e.cause, static_cast<uint8_t>(SuspensionKind::Deopt));
    VORTEX_EXPECT(e.ttsp_ns > 0);
    VORTEX_EXPECT_EQ(e.threads, 1u);
    VORTEX_EXPECT(!e.timed_out);
    VORTEX_EXPECT(hm.release(*tid).has_value());
    VORTEX_EXPECT(hm.unregister_thread(*tid).has_value());
    hm.set_safepoint_log(nullptr);
}

// ---- DoD 13: the timed-out freeze records its full window --------------------------------

// CEP:WHAT: Test: DoD 13 - a rolled-back freeze (a registered slot that never acknowledges) records its FULL window with timed_out true and ttsp/body >= the budget.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(handshake_timeout_records_timed_out_freeze) {
    HandshakeManager hm;
    SafepointLog log;
    hm.set_safepoint_log(&log);
    auto tid = hm.register_thread();
    VORTEX_EXPECT(tid.has_value());
    // The registered slot never acknowledges: the freeze rolls back.
    auto frozen = hm.freeze_all(SuspensionKind::Gc,
                                std::chrono::milliseconds(50));
    VORTEX_EXPECT(!frozen.has_value());
    VORTEX_EXPECT_EQ(log.events().size(), 1u);
    const SafepointEvent& e = log.events()[0];
    VORTEX_EXPECT(e.kind == SafepointEvent::Kind::Freeze);
    VORTEX_EXPECT(e.timed_out);
    VORTEX_EXPECT(e.ttsp_ns >= 50'000'000ull);
    VORTEX_EXPECT(e.body_ns >= 50'000'000ull);
    VORTEX_EXPECT(hm.unregister_thread(*tid).has_value());
    hm.set_safepoint_log(nullptr);
}

// ---- DoD 14: the ring is bounded with honest counters -------------------------------------

// CEP:WHAT: Test: DoD 14 - 600 events against the 512-cap ring keep the newest (600 - 512 remain) while total_events keeps the true count; percentiles stay honest.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(safepoint_log_ring_is_bounded) {
    SafepointLog log;
    for (int i = 0; i < 600; ++i) {
        SafepointEvent e;
        e.kind = (i % 2) == 0 ? SafepointEvent::Kind::Freeze
                              : SafepointEvent::Kind::Handshake;
        e.ttsp_ns = static_cast<uint64_t>(i) * 1000ull;
        log.record(e);
    }
    // Clear-on-full keeps the NEWEST events: 600 recorded, 512-cap ring ->
    // 600 - 512 = 88 remain. total_events keeps the true count.
    VORTEX_EXPECT_EQ(log.events().size(), 600 - SafepointLog::kMaxEvents);
    VORTEX_EXPECT_EQ(log.total_events(), 600ull);
    VORTEX_EXPECT(log.ttsp_p99_ns() > 0);
    VORTEX_EXPECT_EQ(log.ttsp_max_ns(), 599'000ull);
}

// ---- DoD 15: the pause ring carries causes (through a REAL engine) -------------------------

// CEP:WHAT: Test: DoD 15 - the two-arg record_pause overload lands Unspecified; a REAL occupancy-triggered cycle stamps OccupancyTrigger on every phase/slice event and a REAL young GC stamps AllocationSlowPath.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(gc_pause_ring_carries_causes) {
    // Unit level: the overload surface (legacy two-arg lands Unspecified).
    gc::GcStats gs;
    gs.record_pause(gc::PauseKind::MarkSlice, 10);
    VORTEX_EXPECT(gs.pauses.back().cause == gc::PauseCause::Unspecified);
    gs.record_pause(gc::PauseKind::MarkSlice, 20, gc::PauseCause::ExplicitRequest);
    VORTEX_EXPECT(gs.pauses.back().cause == gc::PauseCause::ExplicitRequest);
    VORTEX_EXPECT(std::strcmp(gc::pause_cause_name(gc::PauseCause::OccupancyTrigger),
                              "occupancy_trigger") == 0);

    // Engine level: occupancy-triggered cycle -> OccupancyTrigger on every
    // phase/slice event; young GC -> AllocationSlowPath. The HEAP owns its
    // engine (icggc.cpp: the heap constructs it; collect_young records
    // through heap.engine()).
    Fixture f;
    gc::IcggcEngine& engine = f.heap.engine();
    Klass* k = make_klass(f, "CauseFill", 2);
    while (engine.old_occupancy_pct() < engine.config.old_gc_trigger_pct) {
        auto o = f.heap.allocate_object(k, 2);
        if (!o) break;
        if (fill_old_copy(f.heap, *o) == nullptr) break;
    }
    VORTEX_EXPECT(engine.old_occupancy_pct() >=
                  engine.config.old_gc_trigger_pct);
    auto ran = engine.run_cycle();
    VORTEX_EXPECT(ran.has_value());
    bool saw_initial = false;
    for (const gc::PauseEvent& p : engine.stats().pauses) {
        if (p.kind == gc::PauseKind::InitialMark) saw_initial = true;
        if (p.kind == gc::PauseKind::YoungGc) continue;  // checked below
        VORTEX_EXPECT(p.cause == gc::PauseCause::OccupancyTrigger);
    }
    VORTEX_EXPECT(saw_initial);

    const uint64_t young_before = engine.stats().young_collections;
    for (int i = 0; i < 20000 && engine.stats().young_collections == young_before;
         ++i) {
        auto o = f.heap.allocate_object(k, 2);
        if (!o) break;
    }
    VORTEX_EXPECT(engine.stats().young_collections > young_before);
    bool saw_young = false;
    for (const gc::PauseEvent& p : engine.stats().pauses) {
        if (p.kind != gc::PauseKind::YoungGc) continue;
        saw_young = true;
        VORTEX_EXPECT(p.cause == gc::PauseCause::AllocationSlowPath);
    }
    VORTEX_EXPECT(saw_young);
}

// ---- DoD 16: the allocation sampler ---------------------------------------------------------

// CEP:WHAT: Test: DoD 16 - the sampler fires exactly at interval boundaries (a fresh window per set_interval_bytes), counters are exact, the ring keeps the newest modulo the residue, site_id stays 0, and interval 0 is a named reject.
// CEP:WHY: The observability floor is machine-facing file formats and
//           runtime telemetry - the pin fails loudly on contract drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(alloc_sampler_interval_and_ring) {
    AllocSampler& s = alloc_sampler();
    auto bad = s.set_interval_bytes(0);
    VORTEX_EXPECT(!bad.has_value());

    VORTEX_EXPECT(s.set_interval_bytes(1000).has_value());
    const uint64_t refills_before = s.refills_observed();
    const uint64_t samples_before = s.total_samples();
    s.on_refill(1500);
    VORTEX_EXPECT_EQ(s.refills_observed(), refills_before + 1);
    VORTEX_EXPECT_EQ(s.total_samples(), samples_before + 1);
    VORTEX_EXPECT_EQ(s.samples().back().refill_bytes, 1500ull);
    VORTEX_EXPECT(s.samples().back().timestamp_ns > 0);
    VORTEX_EXPECT_EQ(s.samples().back().site_id, 0u);  // reserved (attribution = R31)
    // No interval crossing: no sample.
    s.on_refill(400);
    VORTEX_EXPECT_EQ(s.total_samples(), samples_before + 1);
    // Crossing again: the accumulation carries over (400 + 700 >= 1000).
    s.on_refill(700);
    VORTEX_EXPECT_EQ(s.total_samples(), samples_before + 2);

    // Ring bound: interval 1 (a fresh window — set_interval_bytes resets the
    // accumulation), 6000 refills -> 6000 samples total; the ring keeps the
    // newest: final size = (residue + 6000) mod cap (one clear fires once
    // the cap is hit; the residue from earlier real refills is accounted).
    VORTEX_EXPECT(s.set_interval_bytes(1).has_value());
    const uint64_t ring_before = s.total_samples();
    const size_t ring_size_before = s.samples().size();
    for (int i = 0; i < 6000; ++i) s.on_refill(1);
    VORTEX_EXPECT_EQ(s.total_samples(), ring_before + 6000);
    VORTEX_EXPECT_EQ(s.samples().size(),
                     (ring_size_before + 6000) % AllocSampler::kMaxSamples);

    VORTEX_EXPECT(s.set_interval_bytes(AllocSampler::kDefaultIntervalBytes)
                      .has_value());
}

// ---- DoD 17 (gate-verified): `vx stats` prints the telemetry summary.
// The unit binary cannot spawn the vx driver portably; the gate script runs
// `vx stats <sample> --entry main` and greps for the alloc_sampler/heap
// lines (docs/infrastructure/06-observability.md DoD 17 note).

// ---- batch e (M6-e): DoD 18-33 -------------------------------------------------
// Execution-order note: the sanitizer policy attach is process-monotonic, so
// the DISABLED-state tests (DoD 27/31) are declared FIRST; the enabling
// attach happens in the DoD-23 test and stays on for the rest of the binary
// (analyzed safe: the producers poison only dead spans, and every consumer
// resolves live bodies before access).

#include <elf.h>

namespace {

// CEP:WHAT: The DoD-26 foreign target (never called — the boundary check is
//           what's under test).
void obs_ffi_probe(void*) {}

// CEP:WHAT: The signature helper (the test_ffi shape, local to this file).
ForeignSignature make_sig(std::vector<ForeignArgKind> args,
                          ForeignReturnKind ret) {
    ForeignSignature s;
    s.args = std::move(args);
    s.return_kind = ret;
    return s;
}


// CEP:WHAT: Walks a GDB symfile image per the emitter's fixed layout
//           (Ehdr/Phdr/5 sections) and returns the .text/.symtab spans.
// CEP:WHY: DoD 19/22 parse the symfile EXACTLY as gdb would — generic
//          section walking, not offset assumptions beyond the format.
// CEP:STATUS: complete
// CEP:FAILURE: returns false on any malformed structure.
// CEP:ASSUMES: ELF64 LE (the emitter's only output).
// CEP:COST: @cold.
// CEP:EVIDENCE: the gdb DoD tests.
struct ParsedSymfile {
    uint64_t text_addr = 0;
    const uint8_t* text = nullptr;
    size_t text_size = 0;
    const uint8_t* symtab = nullptr;
    size_t symtab_bytes = 0;
    const uint8_t* strtab = nullptr;
    size_t strtab_size = 0;
};

bool parse_gdb_symfile(std::span<const uint8_t> img, ParsedSymfile& out) {
    if (img.size() < sizeof(Elf64_Ehdr)) return false;
    const Elf64_Ehdr* eh = reinterpret_cast<const Elf64_Ehdr*>(img.data());
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return false;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB) return false;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) return false;
    if (eh->e_shoff + eh->e_shnum * sizeof(Elf64_Shdr) > img.size())
        return false;
    const Elf64_Shdr* sh = reinterpret_cast<const Elf64_Shdr*>(
        img.data() + eh->e_shoff);
    if (eh->e_shstrndx >= eh->e_shnum) return false;
    const uint8_t* shstr = img.data() + sh[eh->e_shstrndx].sh_offset;
    bool have_text = false, have_sym = false, have_str = false;
    for (unsigned i = 0; i < eh->e_shnum; ++i) {
        const char* name =
            reinterpret_cast<const char*>(shstr + sh[i].sh_name);
        if (std::strcmp(name, ".text") == 0) {
            if (sh[i].sh_offset + sh[i].sh_size > img.size()) return false;
            out.text = img.data() + sh[i].sh_offset;
            out.text_size = sh[i].sh_size;
            out.text_addr = sh[i].sh_addr;
            have_text = true;
        } else if (std::strcmp(name, ".symtab") == 0) {
            if (sh[i].sh_offset + sh[i].sh_size > img.size()) return false;
            out.symtab = img.data() + sh[i].sh_offset;
            out.symtab_bytes = sh[i].sh_size;
            have_sym = true;
        } else if (std::strcmp(name, ".strtab") == 0) {
            if (sh[i].sh_offset + sh[i].sh_size > img.size()) return false;
            out.strtab = img.data() + sh[i].sh_offset;
            out.strtab_size = sh[i].sh_size;
            have_str = true;
        }
    }
    return have_text && have_sym && have_str;
}

// CEP:WHAT: Finds the STT_FUNC/STB_GLOBAL symbol in a parsed symfile.
// CEP:WHY: DoD 19 pins st_value/st_size on the function symbol.
// CEP:STATUS: complete
// CEP:FAILURE: returns nullptr when absent.
// CEP:ASSUMES: the emitter's one-symbol symtab (plus the null symbol).
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 19.
const Elf64_Sym* find_func_sym(const ParsedSymfile& p) {
    const size_t n = p.symtab_bytes / sizeof(Elf64_Sym);
    for (size_t i = 0; i < n; ++i) {
        const Elf64_Sym* s =
            reinterpret_cast<const Elf64_Sym*>(p.symtab) + i;
        if (ELF64_ST_TYPE(s->st_info) == STT_FUNC &&
            ELF64_ST_BIND(s->st_info) == STB_GLOBAL) {
            return s;
        }
    }
    return nullptr;
}

}  // namespace

// DoD 31 — the replay flag is off by default and no producer records.
VORTEX_TEST(replay_disabled_zero_events) {
    VORTEX_EXPECT(!replay_enabled());
    const uint64_t total_before = replay_recorder().total_events();
    replay_recorder().record_event(
        static_cast<uint32_t>(ReplayKind::Publish), 1, 1);
    VORTEX_EXPECT(replay_recorder().events().empty());
    VORTEX_EXPECT_EQ(replay_recorder().total_events(), total_before);
}

// DoD 27 — policy off: poisoned spans are NOT reported (the short-circuit).
VORTEX_TEST(sanitizer_disabled_no_detections) {
    VORTEX_EXPECT(!sanitizer_policy().guest_asan);
    Fixture f;
    gc::IcggcEngine& engine = f.heap.engine();
    Klass* k = make_klass(f, "SanOff", 2);
    while (engine.old_occupancy_pct() < engine.config.old_gc_trigger_pct) {
        auto o = f.heap.allocate_object(k, 2);
        if (!o) break;
        if (fill_old_copy(f.heap, *o) == nullptr) break;
    }
    auto ran = engine.run_cycle();
    VORTEX_EXPECT(ran.has_value());
    if (!ran) return;
    // Some region was evacuated + reset (poisoned in the shadow) — yet the
    // disabled policy short-circuits verify_span: always ok.
    bool probed_a_span = false;
    for (const gc::Region& r : f.heap.regions()) {
        if (r.state == gc::RegionState::Free) {
            auto v = verify_span(r.start, 64);
            VORTEX_EXPECT(v.has_value());
            probed_a_span = true;
            break;
        }
    }
    VORTEX_EXPECT(probed_a_span);
}

// DoD 23 — the reserved bit is refused by name; guest_asan enables cleanly.
VORTEX_TEST(sanitizer_policy_validation_refuses_guest_tsan_named) {
    auto bad = SanitizerPolicy::make(false, true);
    VORTEX_EXPECT(!bad.has_value());
    if (!bad) {
        VORTEX_EXPECT(bad.error().message.find("reserved") !=
                      std::string::npos);
    }
    SanitizerPolicy p;
    auto made = SanitizerPolicy::make(true, false);
    VORTEX_EXPECT(made.has_value());
    if (made) p = *made;
    auto attached = attach_sanitizer_policy(p);
    VORTEX_EXPECT(attached.has_value());
    VORTEX_EXPECT(sanitizer_policy().guest_asan);
    // Identical re-attach is legal; a DIFFERENT policy is a named reject.
    VORTEX_EXPECT(attach_sanitizer_policy(p).has_value());
    SanitizerPolicy q;
    auto made2 = SanitizerPolicy::make(false, false);
    if (made2) q = *made2;
    auto mismatch = attach_sanitizer_policy(q);
    VORTEX_EXPECT(!mismatch.has_value());
}

// DoD 25 — region release poisons; recycle unpoisons (real heap paths).
VORTEX_TEST(sanitizer_released_region_detected) {
    Fixture f;
    Klass* k = make_klass(f, "SanRel", 2);
    // Occupy at least one region.
    gc::Region* target = nullptr;
    for (int i = 0; i < 4096 && target == nullptr; ++i) {
        auto o = f.heap.allocate_object(k, 2);
        VORTEX_EXPECT(o.has_value());
        if (!o) return;
        if (fill_old_copy(f.heap, *o) == nullptr) break;
        for (gc::Region& r : f.heap.regions_mut()) {
            if (r.state == gc::RegionState::Occupied ||
                r.state == gc::RegionState::Bump) {
                target = &r;
                break;
            }
        }
    }
    VORTEX_EXPECT(target != nullptr);
    if (target == nullptr) return;
    // Occupied: the span is addressable.
    VORTEX_EXPECT(verify_span(target->start, 64).has_value());
    f.heap.reset_region_to_free(*target);
    // Released: the named sanitizer failure, kind + address in the message.
    auto v = verify_span(target->start, 64);
    VORTEX_EXPECT(!v.has_value());
    if (!v) {
        VORTEX_EXPECT(v.error().code == support::ErrorCode::GuestSanitizer);
        VORTEX_EXPECT(v.error().message.find("guest sanitizer") !=
                      std::string::npos);
    }
    // Recycle: the next promotion takes the LIFO free region back — the
    // span is addressable again (a reused region must never read poisoned).
    auto young = f.heap.allocate_object(k, 2);
    VORTEX_EXPECT(young.has_value());
    if (!young) return;
    VORTEX_EXPECT(fill_old_copy(f.heap, *young) != nullptr);
    VORTEX_EXPECT(verify_span(target->start, 64).has_value());
}

// DoD 24 — the evacuated region's span is a named trap after a REAL cycle;
// the kEvacuated kind is unit-pinned on a standalone shadow.
VORTEX_TEST(sanitizer_evacuated_region_and_kind) {
    // Unit level: the kind values are the shadow's own contract.
    alignas(64) static uint8_t span[256];
    GuestShadow sh(span, sizeof(span));
    VORTEX_EXPECT(sh.span_ok(span + 8, 16));
    sh.poison(reinterpret_cast<uint64_t>(span) + 8, 16,
              GuestShadow::kEvacuated);
    VORTEX_EXPECT(!sh.span_ok(span + 8, 16));
    VORTEX_EXPECT_EQ(sh.poison_kind(span + 8), GuestShadow::kEvacuated);
    sh.unpoison(reinterpret_cast<uint64_t>(span) + 8, 16);
    VORTEX_EXPECT(sh.span_ok(span + 8, 16));
    VORTEX_EXPECT_EQ(sh.poison_kind(span + 8), GuestShadow::kAddressable);

    // Engine level: a REAL occupancy cycle leaves the evacuated regions'
    // spans non-addressable under the enabled policy.
    Fixture f;
    gc::IcggcEngine& engine = f.heap.engine();
    Klass* k = make_klass(f, "SanEvac", 2);
    while (engine.old_occupancy_pct() < engine.config.old_gc_trigger_pct) {
        auto o = f.heap.allocate_object(k, 2);
        if (!o) break;
        if (fill_old_copy(f.heap, *o) == nullptr) break;
    }
    auto ran = engine.run_cycle();
    VORTEX_EXPECT(ran.has_value());
    if (!ran) return;
    bool saw_released = false;
    for (const gc::Region& r : f.heap.regions()) {
        if (r.state == gc::RegionState::Free && r.epoch > 1) {
            auto v = verify_span(r.start, 64);
            VORTEX_EXPECT(!v.has_value());
            saw_released = true;
            break;
        }
    }
    VORTEX_EXPECT(saw_released);
}

// DoD 26 — the FFI boundary validates Ptr-arg spans (the C++ check).
VORTEX_TEST(sanitizer_ffi_ptr_boundary_validated) {
    gc::Heap heap;
    Klass k("SanFfi");  // heap-owned descriptor is a test-local singleton
    auto young = heap.allocate_object(&k, 0);
    VORTEX_EXPECT(young.has_value());
    if (!young) return;
    auto* old = fill_old_copy(heap, *young);
    VORTEX_EXPECT(old != nullptr);
    if (old == nullptr) return;
    auto stub_r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Ptr}, ForeignReturnKind::Void),
        reinterpret_cast<void*>(&obs_ffi_probe), nullptr);
    VORTEX_EXPECT(stub_r.has_value());
    if (!stub_r) return;
    GuestToNativeStub stub = std::move(*stub_r);
    VORTEX_EXPECT_EQ(stub.ptr_arg_slots.size(), size_t{1});
    const TaggedValue args[1] = {
        TaggedValue::heap_pointer(reinterpret_cast<Object*>(old))};
    // Healthy: the boundary check passes.
    VORTEX_EXPECT(stub.validate_ptr_args(args, 1).has_value());
    // Null passes (the machine gates own the type errors).
    const TaggedValue null_args[1] = {TaggedValue::null()};
    VORTEX_EXPECT(stub.validate_ptr_args(null_args, 1).has_value());
    // Poison the object's region (release): the boundary refuses by name.
    gc::Region* r = heap.region_of(old);
    VORTEX_EXPECT(r != nullptr);
    if (r == nullptr) return;
    heap.reset_region_to_free(*r);
    auto v = stub.validate_ptr_args(args, 1);
    VORTEX_EXPECT(!v.has_value());
    if (!v) {
        VORTEX_EXPECT(v.error().code == support::ErrorCode::GuestSanitizer);
    }
}

// DoD 18 — the descriptor + breakpoint symbols are present.
VORTEX_TEST(gdb_descriptor_and_breakpoint_symbols_present) {
    // The runtime links vortex_core statically — the canonical symbols are
    // resolved at link time (dlsym sees only .dynsym, which a static
    // archive does not populate).
    VORTEX_EXPECT(&__jit_debug_register_code != nullptr);
    VORTEX_EXPECT(&__jit_debug_descriptor != nullptr);
    VORTEX_EXPECT_EQ(__jit_debug_descriptor.version, 1u);
}

// DoD 19 — registration builds a parseable ELF64 symfile.
VORTEX_TEST(gdb_register_builds_parseable_elf_symfile) {
    alignas(16) static const uint8_t code[] = {
        0x55, 0x48, 0x89, 0xe5, 0x5d, 0xc3};  // push rbp; mov..; pop; ret
    auto r = GdbJitInterface::register_code(code, sizeof(code), "vx_test_fn");
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    VORTEX_EXPECT(GdbJitInterface::is_registered(code));
    VORTEX_EXPECT_EQ(__jit_debug_descriptor.action_flag,
                     GdbJitInterface::kJitRegisterFn);
    VORTEX_EXPECT(__jit_debug_descriptor.first_entry != nullptr);
    auto sym = GdbJitInterface::symfile_of(code);
    VORTEX_EXPECT(!sym.empty());
    ParsedSymfile p;
    VORTEX_EXPECT(parse_gdb_symfile(sym, p));
    VORTEX_EXPECT_EQ(p.text_addr, reinterpret_cast<uint64_t>(code));
    VORTEX_EXPECT_EQ(p.text_size, sizeof(code));
    VORTEX_EXPECT(std::memcmp(p.text, code, sizeof(code)) == 0);
    const Elf64_Sym* s = find_func_sym(p);
    VORTEX_EXPECT(s != nullptr);
    if (s != nullptr) {
        VORTEX_EXPECT_EQ(s->st_value, reinterpret_cast<uint64_t>(code));
        VORTEX_EXPECT_EQ(s->st_size, sizeof(code));
        const char* name =
            reinterpret_cast<const char*>(p.strtab + s->st_name);
        VORTEX_EXPECT(std::strcmp(name, "vx_test_fn") == 0);
    }
    VORTEX_EXPECT(GdbJitInterface::unregister_code(code).has_value());
}

// DoD 20 — unregister removes the entry and flips the action.
VORTEX_TEST(gdb_unregister_removes_entry_and_sets_action) {
    alignas(16) static const uint8_t code[] = {0xc3};
    VORTEX_EXPECT(
        GdbJitInterface::register_code(code, sizeof(code), "vx_tmp").has_value());
    VORTEX_EXPECT(GdbJitInterface::unregister_code(code).has_value());
    VORTEX_EXPECT(!GdbJitInterface::is_registered(code));
    VORTEX_EXPECT_EQ(__jit_debug_descriptor.action_flag,
                     GdbJitInterface::kJitUnregisterFn);
    VORTEX_EXPECT(__jit_debug_descriptor.first_entry == nullptr);
}

// DoD 21 — the named rejects.
VORTEX_TEST(gdb_named_failures) {
    alignas(16) static const uint8_t code[] = {0xc3};
    auto dup = GdbJitInterface::register_code(code, sizeof(code), "a");
    VORTEX_EXPECT(dup.has_value());
    auto again = GdbJitInterface::register_code(code, sizeof(code), "a");
    VORTEX_EXPECT(!again.has_value());
    if (!again) {
        VORTEX_EXPECT(again.error().message.find("duplicate") !=
                      std::string::npos);
    }
    auto unknown = GdbJitInterface::unregister_code(code + 64);
    VORTEX_EXPECT(!unknown.has_value());
    auto null_reg = GdbJitInterface::register_code(nullptr, 8, "a");
    VORTEX_EXPECT(!null_reg.has_value());
    auto zero_reg = GdbJitInterface::register_code(code, 0, "a");
    VORTEX_EXPECT(!zero_reg.has_value());
    auto null_un = GdbJitInterface::unregister_code(nullptr);
    VORTEX_EXPECT(!null_un.has_value());
    VORTEX_EXPECT(GdbJitInterface::unregister_code(code).has_value());
}

// DoD 22 — multiple registrations chain head-first; all symfiles parse.
VORTEX_TEST(gdb_multiple_entries_chain_in_order) {
    alignas(16) static const uint8_t c1[] = {0x90, 0xc3};
    alignas(16) static const uint8_t c2[] = {0x91, 0xc3};
    alignas(16) static const uint8_t c3[] = {0x92, 0xc3};
    VORTEX_EXPECT(GdbJitInterface::register_code(c1, sizeof(c1), "f1").has_value());
    VORTEX_EXPECT(GdbJitInterface::register_code(c2, sizeof(c2), "f2").has_value());
    VORTEX_EXPECT(GdbJitInterface::register_code(c3, sizeof(c3), "f3").has_value());
    VORTEX_EXPECT_EQ(GdbJitInterface::live_entries(), size_t{3});
    // Head-first: the newest registration sits at first_entry.
    GdbJitCodeEntry* e = __jit_debug_descriptor.first_entry;
    VORTEX_EXPECT(e != nullptr);
    auto s3 = GdbJitInterface::symfile_of(c3);
    VORTEX_EXPECT(!s3.empty());
    if (e != nullptr) {
        VORTEX_EXPECT_EQ(e->symfile_addr,
                         reinterpret_cast<uint64_t>(s3.data()));
    }
    // Full interior order: c3 (head) -> c2 -> c1 (registration-reverse).
    const auto s2 = GdbJitInterface::symfile_of(c2);
    const auto s1 = GdbJitInterface::symfile_of(c1);
    VORTEX_EXPECT(!s2.empty() && !s1.empty());
    if (e != nullptr) {
        VORTEX_EXPECT_EQ(e->next_entry != nullptr
                             ? e->next_entry->symfile_addr
                             : 0,
                         reinterpret_cast<uint64_t>(s2.data()));
        if (e->next_entry != nullptr) {
            VORTEX_EXPECT_EQ(
                e->next_entry->next_entry != nullptr
                    ? e->next_entry->next_entry->symfile_addr
                    : 0,
                reinterpret_cast<uint64_t>(s1.data()));
            VORTEX_EXPECT_EQ(
                e->next_entry->next_entry->next_entry,
                static_cast<GdbJitCodeEntry*>(nullptr));
            VORTEX_EXPECT_EQ(
                e->next_entry->prev_entry, e);
        }
        VORTEX_EXPECT_EQ(e->prev_entry,
                         static_cast<GdbJitCodeEntry*>(nullptr));
    }
    int chain = 0;
    while (e != nullptr) {
        ParsedSymfile p;
        VORTEX_EXPECT(parse_gdb_symfile(
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(e->symfile_addr),
                e->symfile_size),
            p));
        e = e->next_entry;
        ++chain;
    }
    VORTEX_EXPECT_EQ(chain, 3);
    VORTEX_EXPECT(GdbJitInterface::unregister_code(c1).has_value());
    VORTEX_EXPECT(GdbJitInterface::unregister_code(c2).has_value());
    VORTEX_EXPECT(GdbJitInterface::unregister_code(c3).has_value());
    VORTEX_EXPECT_EQ(GdbJitInterface::live_entries(), size_t{0});
}

namespace {

// CEP:WHAT: The DoD-28 workload: a module whose field sites fill ICs
//           deterministically (two identical builds -> identical streams).
// CEP:WHY: The engine-wide "same program + same input -> same decisions"
//          property needs a REAL interpreter run on both sides.
// CEP:STATUS: complete
// CEP:FAILURE: assemble/verify failures fail the test.
// CEP:ASSUMES: fresh module + heap + interpreter per run (no shared state).
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 28.
vm::Result<vm::RunResult> replay_run_once() {
    auto module = ugb::assemble_module(R"(
.class P
.field x in P

.method make(regs=3, args=1)
    New.Object v1, P
    SetField v1, v0, P.x
    Return v1
.end

.method getx(regs=2, args=1)
    GetField v1, v0, P.x
    Return v1
.end

.method main(regs=4, args=0)
    Const.I32 v0, 5
    Call.Direct v1, v0, 1, make
    Call.Direct v2, v1, 1, getx
    Return v2
.end
)");
    if (!module) return std::unexpected(module.error());
    gc::Heap heap;
    vm::Interpreter interp(heap);
    return interp.run(*module, "main", {});
}

}  // namespace

// DoD 28 — two identical runs produce identical decision streams.
VORTEX_TEST(replay_identical_runs_identical_streams) {
    set_replay_enabled(true);
    ReplayRecorder local1;
    VORTEX_EXPECT(replay_run_once().has_value());
    for (const ReplayEvent& e : replay_recorder().events()) {
        local1.record_event(e.kind, e.a, e.b);
    }
    const size_t n1 = local1.events().size();
    replay_recorder().clear();
    VORTEX_EXPECT(replay_run_once().has_value());
    ReplayRecorder local2;
    for (const ReplayEvent& e : replay_recorder().events()) {
        local2.record_event(e.kind, e.a, e.b);
    }
    replay_recorder().clear();
    set_replay_enabled(false);
    VORTEX_EXPECT(n1 > 0);  // the IC producers fired on both runs
    auto d = compare_streams(local1, local2);
    VORTEX_EXPECT(d.identical);
}

// DoD 29 — the first divergence is located with both payloads.
VORTEX_TEST(replay_first_divergence_located) {
    ReplayRecorder a;
    ReplayRecorder b;
    set_replay_enabled(true);
    for (int i = 0; i < 4; ++i) {
        a.record_event(static_cast<uint32_t>(ReplayKind::Publish), i, 1);
        b.record_event(static_cast<uint32_t>(ReplayKind::Publish), i, 1);
    }
    b.record_event(static_cast<uint32_t>(ReplayKind::Deopt), 7, 2);
    a.record_event(static_cast<uint32_t>(ReplayKind::Publish), 7, 2);
    set_replay_enabled(false);
    auto d = compare_streams(a, b);
    VORTEX_EXPECT(!d.identical);
    VORTEX_EXPECT_EQ(d.first_divergence, size_t{4});
    VORTEX_EXPECT_EQ(d.kind, static_cast<uint32_t>(ReplayKind::Publish));
    VORTEX_EXPECT_EQ(d.expected_a, 7ull);
    VORTEX_EXPECT_EQ(d.actual_a, 7ull);
    VORTEX_EXPECT_EQ(d.expected_b, 2ull);
    VORTEX_EXPECT_EQ(d.actual_b, 2ull);
    // A kind mismatch at the same index also reports.
    ReplayRecorder c;
    set_replay_enabled(true);
    c.record_event(static_cast<uint32_t>(ReplayKind::OsrEntry), 1, 2);
    set_replay_enabled(false);
    ReplayRecorder e2;
    set_replay_enabled(true);
    e2.record_event(static_cast<uint32_t>(ReplayKind::Publish), 1, 2);
    set_replay_enabled(false);
    auto d2 = compare_streams(c, e2);
    VORTEX_EXPECT(!d2.identical);
    VORTEX_EXPECT_EQ(d2.first_divergence, size_t{0});
    // A same-kind payload mismatch also reports (the comparator cannot be
    // payload-blind).
    ReplayRecorder f1;
    ReplayRecorder f2;
    set_replay_enabled(true);
    f1.record_event(static_cast<uint32_t>(ReplayKind::Publish), 11, 22);
    f2.record_event(static_cast<uint32_t>(ReplayKind::Publish), 99, 22);
    set_replay_enabled(false);
    auto d4 = compare_streams(f1, f2);
    VORTEX_EXPECT(!d4.identical);
    VORTEX_EXPECT_EQ(d4.first_divergence, size_t{0});
    VORTEX_EXPECT_EQ(d4.expected_a, 11ull);
    VORTEX_EXPECT_EQ(d4.actual_a, 99ull);
    // A length mismatch diverges at the shared length.
    auto d3 = compare_streams(a, b);
    VORTEX_EXPECT(!d3.identical);
}

// DoD 30 — the ring is bounded (Rule 28); seq/total survive the clear.
VORTEX_TEST(replay_ring_bounded_rule28) {
    set_replay_enabled(true);
    replay_recorder().clear();
    const uint64_t total_before = replay_recorder().total_events();
    for (int i = 0; i < 70000; ++i) {
        replay_recorder().record_event(
            static_cast<uint32_t>(ReplayKind::Publish), 0, 0);
    }
    VORTEX_EXPECT_EQ(replay_recorder().events().size(),
                     70000 % ReplayRecorder::kMaxEvents);
    VORTEX_EXPECT_EQ(replay_recorder().total_events(), total_before + 70000);
    // seq is monotonic across the clear.
    const auto& ev = replay_recorder().events();
    VORTEX_EXPECT(ev.back().seq > ev.front().seq);
    replay_recorder().clear();
    set_replay_enabled(false);
}

namespace {

// CEP:WHAT: The DoD-32 J1 compile fixture: a corpus + a module whose body
//           carries Debug.SourcePosition hints.
// CEP:WHY: The compiler's line-map stamping needs a REAL compile.
// CEP:STATUS: complete
// CEP:FAILURE: compile failures fail the test (no silent pass).
// CEP:ASSUMES: the default corpus covers the zero-width DEBUG_SRCPOS
//          stencil (batch e addition).
// CEP:COST: @cold.
// CEP:EVIDENCE: DoD 32.
ugb::UGBModule make_lined_module() {
    ugb::UGBModule module;
    ugb::MethodBuilder b(module, "lined_fn", 4, 0);
    b.emit(ugb::Op::DEBUG_SRCPOS, 0, {}, true, 10);
    b.const_i32(1, 3);
    b.emit(ugb::Op::DEBUG_SRCPOS, 0, {}, true, 20);
    b.emit(ugb::Op::ADD_I32, 2, {1, 1});
    b.emit(ugb::Op::DEBUG_SRCPOS, 0, {}, true, 30);
    b.ret(2);
    auto ok = b.finish();
    VORTEX_EXPECT(ok.has_value());
    return module;
}

}  // namespace

// DoD 32 — real names + line entries through the J1 publish path.
VORTEX_TEST(jitdump_real_name_and_debug_entries) {
    const std::string jit = temp_path(".jitdump");
    const std::string map = temp_path(".map");
    JitSession& s = jit_session();
    VORTEX_EXPECT(s.start(jit, map).has_value());

    j1::StencilTable corpus;
    j1::build_default_corpus(corpus);
    ugb::UGBModule module = make_lined_module();
    auto verified = ugb::verify_module(module);
    VORTEX_EXPECT(verified.has_value());
    gc::Heap heap;
    vm::Interpreter interp{heap};
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) {
        VORTEX_EXPECT(s.stop().has_value());
        return;
    }
    const int32_t id = module.find_method("lined_fn");
    VORTEX_EXPECT(id >= 0);
    if (id < 0) {
        VORTEX_EXPECT(s.stop().has_value());
        return;
    }
    j1::BaselineJit compiler(corpus);
    j1::BaselineJob job;
    job.module = &module;
    job.method_id = static_cast<uint32_t>(id);
    job.klass_addrs = &bindings.klass_addr_table;
    job.field_offsets = bindings.field_offsets.data();
    job.field_offset_count = bindings.field_offsets.size();
    job.double_klass = bindings.double_klass;
    auto code = compiler.compile(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) {
        VORTEX_EXPECT(s.stop().has_value());
        return;
    }
    // The compiler stamped the real symbol + the exact line map.
    VORTEX_EXPECT(code->symbol_name == "lined_fn");
    VORTEX_EXPECT_EQ(code->line_map.size(), size_t{3});
    auto ex = j1::publish_baseline(*code, nullptr);
    VORTEX_EXPECT(ex.has_value());
    if (!ex) {
        VORTEX_EXPECT(s.stop().has_value());
        return;
    }
    VORTEX_EXPECT(s.stop().has_value());

    ParsedJitdump p;
    VORTEX_EXPECT(parse_jitdump(jit, p));
    const ParsedRecord* load = nullptr;
    for (const auto& rec : p.records) {
        if (rec.id == 0) load = &rec;  // the last CODE_LOAD wins
    }
    VORTEX_EXPECT(load != nullptr);
    if (load == nullptr) return;
    // name + nr_debug_entries: framing(16) + pid(4) + tid(4) + vma(8) +
    // code_addr(8) + code_size(8) + code_index(8) = 40 bytes of payload
    // before the name.
    const std::vector<uint8_t>& b = load->payload;
    VORTEX_EXPECT(b.size() > 40);
    const std::string name(reinterpret_cast<const char*>(b.data()) + 40);
    VORTEX_EXPECT(name == "lined_fn");
    const uint32_t nr = le_u32(b, 40 + name.size() + 1);
    VORTEX_EXPECT_EQ(nr, 3u);
    // Each entry: u64 addr, u32 line, u32 column.
    size_t off = 40 + name.size() + 1 + 4;
    const uint64_t code_addr = le_u64(b, 16);
    uint32_t lines[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        const uint64_t addr = le_u64(b, off);
        lines[i] = le_u32(b, off + 8);
        VORTEX_EXPECT_EQ(le_u32(b, off + 12), 0u);  // column
        VORTEX_EXPECT(addr >= code_addr);
        VORTEX_EXPECT(addr < code_addr + code->code.size());
        off += 16;
    }
    VORTEX_EXPECT_EQ(lines[0], 10u);
    VORTEX_EXPECT_EQ(lines[1], 20u);
    VORTEX_EXPECT_EQ(lines[2], 30u);

    // The family path: a named J2 shape publishes its real name, zero
    // debug entries (the documented deviation).
    VORTEX_EXPECT(s.start(jit, map).has_value());
    j2::J2Code jc;
    jc.method_id = 6;
    jc.symbol_name = "family_fn";
    jc.code = {0xC3};
    jc.entry_offset = 0;
    auto ex2 = j2::publish_j2(jc, nullptr);
    VORTEX_EXPECT(ex2.has_value());
    VORTEX_EXPECT(s.stop().has_value());
    ParsedJitdump p2;
    VORTEX_EXPECT(parse_jitdump(jit, p2));
    const ParsedRecord* load2 = nullptr;
    for (const auto& rec : p2.records) {
        if (rec.id == 0) load2 = &rec;
    }
    VORTEX_EXPECT(load2 != nullptr);
    if (load2 != nullptr) {
        const std::vector<uint8_t>& b2 = load2->payload;
        const std::string name2(
            reinterpret_cast<const char*>(b2.data()) + 40);
        VORTEX_EXPECT(name2 == "family_fn");
        const uint32_t nr2 = le_u32(b2, 40 + name2.size() + 1);
        VORTEX_EXPECT_EQ(nr2, 0u);
    }
    std::remove(jit.c_str());
    std::remove(map.c_str());
}

// DoD 33 — the perf map carries the sanitized real name; the synthetic
// fallback keeps the M6-d shape.
VORTEX_TEST(publish_perfmap_uses_sanitized_real_name) {
    const std::string jit = temp_path(".jitdump");
    const std::string map = temp_path(".map");
    JitSession& s = jit_session();
    VORTEX_EXPECT(s.start(jit, map).has_value());

    j1::BaselineCode bc;
    bc.method_id = 5;
    bc.symbol_name = "my method";
    bc.code = {0xC3};
    bc.osr_entry_offset = 0xFFFFFFFFu;
    auto ex1 = j1::publish_baseline(bc, nullptr);
    VORTEX_EXPECT(ex1.has_value());

    j1::BaselineCode bc2;
    bc2.method_id = 9;  // empty name -> the synthetic fallback
    bc2.code = {0xC3};
    bc2.osr_entry_offset = 0xFFFFFFFFu;
    auto ex2 = j1::publish_baseline(bc2, nullptr);
    VORTEX_EXPECT(ex2.has_value());
    VORTEX_EXPECT(s.stop().has_value());

    std::FILE* f = std::fopen(map.c_str(), "rb");
    VORTEX_EXPECT(f != nullptr);
    if (f != nullptr) {
        char buf[512];
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        std::fclose(f);
        VORTEX_EXPECT(std::strstr(buf, "my_method") != nullptr);
        VORTEX_EXPECT(std::strstr(buf, "vx_j1_m9") != nullptr);
    }
    std::remove(jit.c_str());
    std::remove(map.c_str());
}
