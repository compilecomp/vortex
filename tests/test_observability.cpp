// CEP:FILE: tests/test_observability.cpp
// CEP:WHAT: Infra 6 observability-floor test suite (M6-d, docs/
//           infrastructure/06-observability.md DoD 1-16): the perf-jitdump
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
