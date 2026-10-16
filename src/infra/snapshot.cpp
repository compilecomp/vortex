// CEP:FILE: src/infra/snapshot.cpp
// CEP:WHAT: Implementation of the snapshot container, the Rule-97/98
//           manifest protocol, the heap snapshot writer/reader, the shared
//           code cache, profile persistence, and the PGO-AOT pipeline
//           (docs/infrastructure/03-snapshots-aot.md).
// CEP:WHY: Startup acceleration without a second compilation contract —
//           every reload publishes through the JIT's own W^X path.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Every failure is a named Diagnostic ("snapshot: ..."/
//           "aot: ..."); no artifact can crash the loader (Rule 9) — all
//           reads are bounds-checked through Cursor, and restore failures
//           leave no partial runtime state behind.
// CEP:ASSUMES: Artifacts are untrusted bytes. The heap writer runs at
//           quiescence (Rule 24); the source heap's object chains are
//           intact (header sizes trustworthy — the same assumption the
//           collector's walks make).
// CEP:COST: @cold. Linear in artifact bytes (two hash passes), O(objects)
//           for heap capture/restore (one FlatHashMap build + one
//           allocation pass). No hot-path presence.
// CEP:EVIDENCE: tests/test_snapshot.cpp (container rejects, manifest
//           negatives, heap parity + determinism, code-cache reload parity,
//           profile round trips, AOT determinism + load parity).
#include "vortex/infra/snapshot.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

#include "vortex/deopt/rbpd.hpp"
#include "vortex/gc/icggc.hpp"
#include "vortex/infra/cpu_dispatch.hpp"
#include "vortex/ir/escape_summary.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/j2/passes.hpp"
#include "vortex/j4/max_jit.hpp"
#include "vortex/runtime/object_model.hpp"
#include "vortex/support/containers.hpp"
#include "vortex/support/tagged_value.hpp"
#include "vortex/ugb/module.hpp"
#include "vortex/ugb/verifier.hpp"

namespace vortex::infra {

using support::Diagnostic;
using support::ErrorCode;
using support::FlatHashMap;
using support::Result;

namespace {

// ---- little-endian byte helpers ------------------------------------------

/// CEP:WHAT: Appends one little-endian u16 to the payload buffer.
/// CEP:WHY: The container's byte layout is fixed and little-endian (docs/infrastructure/03 section 1).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure append).
/// CEP:ASSUMES: out is a grow-only build buffer.
/// CEP:COST: @cold. Two byte stores.
/// CEP:EVIDENCE: the container round trips in tests/test_snapshot.cpp.
void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

/// CEP:WHAT: Appends one little-endian u32.
/// CEP:WHY: Section sizes, counts, and offsets are u32/u64 fields in the container.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure append).
/// CEP:ASSUMES: out is a grow-only build buffer.
/// CEP:COST: @cold. Four byte stores.
/// CEP:EVIDENCE: the container round trips in tests/test_snapshot.cpp.
void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

/// CEP:WHAT: Appends one little-endian u64 (offsets, hashes, raw tagged words).
/// CEP:WHY: The container's byte layout is fixed and little-endian.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure append).
/// CEP:ASSUMES: out is a grow-only build buffer.
/// CEP:COST: @cold. Eight byte stores.
/// CEP:EVIDENCE: the container round trips in tests/test_snapshot.cpp.
void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

/// CEP:WHAT: Appends a length-prefixed string (u32 length + bytes).
/// CEP:WHY: Manifest fields and klass/section names use this encoding.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure append).
/// CEP:ASSUMES: s.size() fits u32 (names are short by construction).
/// CEP:COST: @cold. One length store + a byte copy.
/// CEP:EVIDENCE: the manifest round trips in tests/test_snapshot.cpp.
void put_str(std::vector<uint8_t>& out, std::string_view s) {
    put_u32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

/// Bounds-checked cursor over untrusted artifact bytes (Rule 9: a malformed
/// artifact produces a named reject, never an out-of-bounds read).
struct Cursor {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    bool ok = true;

    explicit Cursor(std::span<const uint8_t> s)
        : p(s.data()), end(s.data() + s.size()) {
        ok = true;  // a fresh cursor over the byte window starts valid
    }

/// CEP:WHAT: Bytes left before the cursor's end.
/// CEP:WHY: Every bounded read gates on it (Rule 9: untrusted artifact bytes).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure query).
/// CEP:ASSUMES: only meaningful while ok is true.
/// CEP:COST: @cold. One subtraction.
/// CEP:EVIDENCE: exercised by every malformed-artifact reject in tests/test_snapshot.cpp.
    size_t remaining() const noexcept {
        return ok ? static_cast<size_t>(end - p) : 0;
    }

/// CEP:WHAT: Bounded read of n bytes; flips ok and returns false when exhausted.
/// CEP:WHY: The reader NEVER dereferences past the artifact (Rule 9) — this is the gate.
/// CEP:STATUS: complete
/// CEP:FAILURE: returns false (and poisons the cursor) on any out-of-bounds read.
/// CEP:ASSUMES: dst has room for n bytes when dst is non-null.
/// CEP:COST: @cold. One bounds compare + a copy.
/// CEP:EVIDENCE: the malformed-container rejects in tests/test_snapshot.cpp.
    bool take(uint8_t* dst, size_t n) {
        if (!ok || static_cast<size_t>(end - p) < n) {
            ok = false;
            return false;
        }
        if (dst != nullptr) std::memcpy(dst, p, n);
        p += n;
        return true;
    }

/// CEP:WHAT: Bounded u8 read through the cursor.
/// CEP:WHY: Section kinds and flags are u8 fields.
/// CEP:STATUS: complete
/// CEP:FAILURE: a poisoned cursor returns 0 with ok=false.
/// CEP:ASSUMES: the cursor is checked after the read sequence.
/// CEP:COST: @cold. One load.
/// CEP:EVIDENCE: the section-kind reject in tests/test_snapshot.cpp.
    uint8_t u8() {
        uint8_t v = 0;
        take(&v, 1);
        return v;
    }
/// CEP:WHAT: Bounded little-endian u16 read.
/// CEP:WHY: Escape-location vregs and name lengths are u16 fields.
/// CEP:STATUS: complete
/// CEP:FAILURE: a poisoned cursor returns 0 with ok=false.
/// CEP:ASSUMES: the cursor is checked after the read sequence.
/// CEP:COST: @cold. Two loads + a shift.
/// CEP:EVIDENCE: the region reload in tests/test_snapshot.cpp.
    uint16_t u16() {
        uint8_t b[2] = {0, 0};
        take(b, 2);
        return static_cast<uint16_t>(b[0]) |
               static_cast<uint16_t>(b[1]) << 8;
    }
/// CEP:WHAT: Bounded little-endian u32 read.
/// CEP:WHY: Counts, sizes, and ids are u32 fields.
/// CEP:STATUS: complete
/// CEP:FAILURE: a poisoned cursor returns 0 with ok=false.
/// CEP:ASSUMES: the cursor is checked after the read sequence.
/// CEP:COST: @cold. Four loads.
/// CEP:EVIDENCE: the container and heap reloads in tests/test_snapshot.cpp.
    uint32_t u32() {
        uint8_t b[4] = {0, 0, 0, 0};
        take(b, 4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[i]) << (8 * i);
        return v;
    }
/// CEP:WHAT: Bounded little-endian u64 read.
/// CEP:WHY: Offsets, hashes, and tagged words are u64 fields.
/// CEP:STATUS: complete
/// CEP:FAILURE: a poisoned cursor returns 0 with ok=false.
/// CEP:ASSUMES: the cursor is checked after the read sequence.
/// CEP:COST: @cold. Eight loads.
/// CEP:EVIDENCE: the container and heap reloads in tests/test_snapshot.cpp.
    uint64_t u64() {
        uint8_t b[8] = {0};
        take(b, 8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[i]) << (8 * i);
        return v;
    }
/// CEP:WHAT: Bounded length-prefixed string read.
/// CEP:WHY: Manifest fields and klass/section names use this encoding; an oversized or truncated length poisons the cursor.
/// CEP:STATUS: complete
/// CEP:FAILURE: returns {} and ok=false on any bounds violation.
/// CEP:ASSUMES: the caller checks ok before trusting the result.
/// CEP:COST: @cold. One length read + a copy.
/// CEP:EVIDENCE: the manifest negatives in tests/test_snapshot.cpp.
    std::string str() {
        const uint32_t n = u32();
        if (!ok || n > remaining()) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
};

Diagnostic snap_err(const std::string& what) {
    return Diagnostic{ErrorCode::DecodeError, "snapshot: " + what};
}

Diagnostic aot_err(const std::string& what) {
    return Diagnostic{ErrorCode::DecodeError, "aot: " + what};
}

std::span<const uint8_t> u8_span(const std::vector<uint8_t>& v) {
    return std::span<const uint8_t>(v.data(), v.size());
}

constexpr uint64_t kSwizzleHi = 0xFFFF'FFFFull;  // high word of a swizzled ref
// The swizzle tag occupies the low nibble with a value NO legal TaggedValue
// can take: bit0==0 is Smi, 0x1 heap, 0x3 null, 0x7 undefined, 0xB false,
// 0xF true. 0x5 (0b0101) is outside the scheme, and Smis (including
// negatives, whose high word is 0xFFFF'FFFF) can never decode as swizzled
// because the low nibble differs (review finding 15-e-a/1).
constexpr uint64_t kSwizzleTag = 0x5;
constexpr uint64_t kHeapTagBits = 0x1;           // TaggedValue heap tag nibble

bool is_heap_word(uint64_t bits) noexcept {
    return (bits & 0xF) == kHeapTagBits;
}

bool is_swizzled_word(uint64_t bits) noexcept {
    return (bits >> 32) == kSwizzleHi && (bits & 0xF) == kSwizzleTag;
}

/// The swizzle index out of a recognized swizzled word. The index lives in
/// bits 4..31 (28 bits — 268M objects, above the 1M restore cap), below the
/// fixed high word.
uint32_t swizzle_index(uint64_t bits) noexcept {
    return static_cast<uint32_t>((bits >> 4) & 0x0FFFFFFFull);
}

}  // namespace

// ---- hash -----------------------------------------------------------------

/// CEP:WHAT: FNV-1a 64 — the container's only hash (section payloads + the file).
/// CEP:WHY: One hash function across all integrity levels keeps the Rule-9 protocol auditable (docs/infrastructure/03 section 1).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure function).
/// CEP:ASSUMES: data covers size bytes.
/// CEP:COST: @cold. One multiply+_xor per byte over artifact-sized inputs.
/// CEP:EVIDENCE: the file-hash and section-hash rejects in tests/test_snapshot.cpp.
uint64_t fnv1a64(const uint8_t* data, size_t size) noexcept {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

namespace {

/// CEP:WHAT: FNV-1a 64 over a byte vector (the AOT dependency fingerprints).
/// CEP:WHY: Manifest dependency fingerprints hash the exact input files (docs/infrastructure/03 section 6).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure function).
/// CEP:ASSUMES: v is the exact byte sequence being fingerprinted.
/// CEP:COST: @cold. Linear in v.
/// CEP:EVIDENCE: aot_pipeline_deterministic in tests/test_snapshot.cpp.
uint64_t fnv_bytes(const std::vector<uint8_t>& v) {
    return fnv1a64(v.data(), v.size());
}

/// CEP:WHAT: FNV-1a 64 over a string view.
/// CEP:WHY: Build-identity strings (the J4 stage-driver descriptor) hash through the same function.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure function).
/// CEP:ASSUMES: s is the canonical identity string.
/// CEP:COST: @cold. Linear in s.
/// CEP:EVIDENCE: snapshot_default_manifest's pipeline hash.
uint64_t fnv_str(std::string_view s) {
    return fnv1a64(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace

// ---- the manifest ----------------------------------------------------------

/// CEP:WHAT: Serializes the Rule-97 manifest in its fixed field order.
/// CEP:WHY: The manifest is the artifact's compatibility identity (docs/infrastructure/03 section 2).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure serialization).
/// CEP:ASSUMES: dependency names are short (u32 length prefix).
/// CEP:COST: @cold. Linear in the manifest size.
/// CEP:EVIDENCE: snapshot_manifest_mismatch_rejected in tests/test_snapshot.cpp.
std::vector<uint8_t> build_manifest_payload(const Manifest& m) {
    std::vector<uint8_t> out;
    out.reserve(128 + m.dependencies.size() * 24);
    put_u32(out, m.ugb_version);
    put_u32(out, m.compiler_version);
    put_u64(out, m.pass_pipeline_hash);
    put_str(out, m.target_arch);
    put_str(out, m.target_feature_set);
    put_u64(out, m.abi_hash);
    put_u64(out, m.runtime_config_hash);
    put_u32(out, static_cast<uint32_t>(m.dependencies.size()));
    for (const auto& d : m.dependencies) {
        put_str(out, d.name);
        put_u64(out, d.fingerprint);
    }
    put_u32(out, m.security_policy_version);
    return out;
}

/// CEP:WHAT: Parses a manifest payload with full bounds checking.
/// CEP:WHY: Rule 9: an untrusted manifest is rejected by name, never over-read.
/// CEP:STATUS: complete
/// CEP:FAILURE: named reject on any structure violation (truncation, oversize counts, trailing bytes).
/// CEP:ASSUMES: payload is the raw manifest section body.
/// CEP:COST: @cold. Linear in the payload.
/// CEP:EVIDENCE: snapshot_manifest_mismatch_rejected in tests/test_snapshot.cpp.
Result<Manifest> parse_manifest_payload(std::span<const uint8_t> payload) {
    Cursor c(payload);
    Manifest m;
    m.ugb_version = c.u32();
    m.compiler_version = c.u32();
    m.pass_pipeline_hash = c.u64();
    m.target_arch = c.str();
    m.target_feature_set = c.str();
    m.abi_hash = c.u64();
    m.runtime_config_hash = c.u64();
    const uint32_t dep_count = c.u32();
    if (!c.ok || dep_count > 4096) {
        return std::unexpected(snap_err("manifest structure invalid"));
    }
    m.dependencies.resize(dep_count);
    for (auto& d : m.dependencies) {
        d.name = c.str();
        d.fingerprint = c.u64();
    }
    m.security_policy_version = c.u32();
    if (!c.ok || c.remaining() != 0) {
        return std::unexpected(snap_err("manifest structure invalid"));
    }
    return m;
}

/// CEP:WHAT: True when every artifact feature appears in the loader's set.
/// CEP:WHY: Rule 98's target-feature-support check is a SUBSET relation, not equality (the loader may have more).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure predicate).
/// CEP:ASSUMES: features are space-separated tokens; 'base' is universally available.
/// CEP:COST: @cold. O(tokens^2) over short strings.
/// CEP:EVIDENCE: snapshot_manifest_mismatch_rejected's feature negatives in tests/test_snapshot.cpp.
bool feature_set_subset(std::string_view artifact, std::string_view loader) {
    if (artifact.empty() || artifact == "base") {
        return true;  // the portable set runs everywhere the loader does
    }
    size_t pos = 0;
    while (pos < artifact.size()) {
        const size_t space = artifact.find(' ', pos);
        const auto tok = artifact.substr(
            pos, space == std::string_view::npos ? artifact.size() - pos
                                                 : space - pos);
        if (!tok.empty()) {
            // Every artifact feature must appear in the loader's set.
            bool found = false;
            size_t lpos = 0;
            while (lpos < loader.size()) {
                const size_t lspace = loader.find(' ', lpos);
                const auto ltok = loader.substr(
                    lpos, lspace == std::string_view::npos ? loader.size() - lpos
                                                           : lspace - lpos);
                if (ltok == tok) {
                    found = true;
                    break;
                }
                lpos = lspace == std::string_view::npos ? loader.size()
                                                        : lspace + 1;
            }
            if (!found) return false;
        }
        pos = space == std::string_view::npos ? artifact.size() : space + 1;
    }
    return true;
}

/// CEP:WHAT: Rule-98 field-by-field verification naming the FIRST mismatch.
/// CEP:WHY: An artifact that loads on a mismatched runtime is a silent ABI break; every mismatch must be spellable (Rules 76/98).
/// CEP:STATUS: complete
/// CEP:FAILURE: named diagnostic carrying the field name and both values.
/// CEP:ASSUMES: expected carries the LOADER's identity.
/// CEP:COST: @cold. Field compares + one subset pass.
/// CEP:EVIDENCE: snapshot_manifest_mismatch_rejected (one negative per field) in tests/test_snapshot.cpp.
Result<void> ManifestVerifier::verify(const Manifest& actual,
                                      const Manifest& expected) {
    if (actual.ugb_version != expected.ugb_version) {
        return std::unexpected(snap_err(
            "manifest ugb_version mismatch: have " +
            std::to_string(actual.ugb_version) + " want " +
            std::to_string(expected.ugb_version)));
    }
    if (actual.compiler_version != expected.compiler_version) {
        return std::unexpected(snap_err(
            "manifest compiler_version mismatch: have " +
            std::to_string(actual.compiler_version) + " want " +
            std::to_string(expected.compiler_version)));
    }
    if (actual.pass_pipeline_hash != expected.pass_pipeline_hash) {
        return std::unexpected(snap_err(
            "manifest pass_pipeline_hash mismatch: have " +
            std::to_string(actual.pass_pipeline_hash) + " want " +
            std::to_string(expected.pass_pipeline_hash)));
    }
    if (actual.target_arch != expected.target_arch) {
        return std::unexpected(snap_err("manifest target_arch mismatch: have " +
                                        actual.target_arch + " want " +
                                        expected.target_arch));
    }
    if (!feature_set_subset(actual.target_feature_set,
                            expected.target_feature_set)) {
        return std::unexpected(snap_err(
            "manifest target_feature_set mismatch: have " +
            actual.target_feature_set + " want a subset of " +
            expected.target_feature_set));
    }
    if (actual.abi_hash != expected.abi_hash) {
        return std::unexpected(snap_err(
            "manifest abi_hash mismatch: have " +
            std::to_string(actual.abi_hash) + " want " +
            std::to_string(expected.abi_hash)));
    }
    if (actual.runtime_config_hash != expected.runtime_config_hash) {
        return std::unexpected(snap_err(
            "manifest runtime_config_hash mismatch: have " +
            std::to_string(actual.runtime_config_hash) + " want " +
            std::to_string(expected.runtime_config_hash)));
    }
    // Dependency validity (Rule 98): match BY NAME (order is not part of
    // the contract), then compare fingerprints. Same count required.
    if (actual.dependencies.size() != expected.dependencies.size()) {
        return std::unexpected(snap_err(
            "manifest dependency_fingerprints mismatch: have " +
            std::to_string(actual.dependencies.size()) + " want " +
            std::to_string(expected.dependencies.size())));
    }
    for (const auto& e : expected.dependencies) {
        const ManifestDependency* found = nullptr;
        for (const auto& a : actual.dependencies) {
            if (a.name == e.name) {
                found = &a;
                break;
            }
        }
        if (found == nullptr) {
            return std::unexpected(snap_err(
                "manifest dependency_fingerprints mismatch: dependency " +
                e.name + " missing"));
        }
        if (found->fingerprint != e.fingerprint) {
            return std::unexpected(snap_err(
                "manifest dependency_fingerprints mismatch: dependency " +
                e.name + " have " + std::to_string(found->fingerprint) +
                " want " + std::to_string(e.fingerprint)));
        }
    }
    if (actual.security_policy_version != expected.security_policy_version) {
        return std::unexpected(snap_err(
            "manifest security_policy_version mismatch: have " +
            std::to_string(actual.security_policy_version) + " want " +
            std::to_string(expected.security_policy_version)));
    }
    return support::ok();
}

/// CEP:WHAT: Builds this build's Rule-97 identity (versions, pipeline/ABI/config hashes, CPU feature set).
/// CEP:WHY: Writers and loaders must derive the same identity from the same build; the hashes cover the stable constants the compiled code depends on (docs/infrastructure/03 section 2).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure builder).
/// CEP:ASSUMES: the ABI/config constant set matches the spec's list.
/// CEP:COST: @cold. A few constant hashes + one CPUID.
/// CEP:EVIDENCE: every snapshot test's manifest round trip.
Manifest snapshot_default_manifest() {
    Manifest m;
    m.pass_pipeline_hash =
        fnv_str("vortex-j4-driver:60-named-stages:v1");
    // ABI identity: the frame-geometry constants every published method's
    // metadata depends on (docs/infrastructure/03 section 2).
    const uint32_t spill_base = j2::kJ2SpillBase;
    const uint64_t max_frames = j2::kMaxDeoptFrames;
    const uint64_t tagged_size = sizeof(TaggedValue);
    const uint64_t header_size = sizeof(ObjectHeader);
    const uint8_t abi_blob[sizeof(spill_base) + sizeof(max_frames) +
                           sizeof(tagged_size) + sizeof(header_size)] = {0};
    size_t off = 0;
    std::memcpy(const_cast<uint8_t*>(abi_blob) + off, &spill_base,
                sizeof(spill_base));
    off += sizeof(spill_base);
    std::memcpy(const_cast<uint8_t*>(abi_blob) + off, &max_frames,
                sizeof(max_frames));
    off += sizeof(max_frames);
    std::memcpy(const_cast<uint8_t*>(abi_blob) + off, &tagged_size,
                sizeof(tagged_size));
    off += sizeof(tagged_size);
    std::memcpy(const_cast<uint8_t*>(abi_blob) + off, &header_size,
                sizeof(header_size));
    m.abi_hash = fnv1a64(abi_blob, sizeof(abi_blob));
    // Runtime-config identity: the heap geometry the klass-handle address
    // determinism depends on.
    const size_t young = gc::Heap::kDefaultYoungBytes;
    const size_t old = gc::Heap::kDefaultOldBytes;
    const uint8_t cfg_blob[sizeof(young) + sizeof(old)] = {0};
    std::memcpy(const_cast<uint8_t*>(cfg_blob), &young, sizeof(young));
    std::memcpy(const_cast<uint8_t*>(cfg_blob) + sizeof(young), &old,
                sizeof(old));
    m.runtime_config_hash = fnv1a64(cfg_blob, sizeof(cfg_blob));
    const CpuInfo cpu = detect_cpu();
    m.target_feature_set = feature_level_name(feature_level(cpu.features));
    return m;
}

// ---- the container writer --------------------------------------------------

/// CEP:WHAT: Shared section registration (dedup + counters).
/// CEP:WHY: Duplicate kind+name pairs would make section lookup ambiguous; reuse after finalize would corrupt the layout.
/// CEP:STATUS: complete
/// CEP:FAILURE: named reject on duplicate kind+name or reuse after finalize.
/// CEP:ASSUMES: called through add_section/add_section_named.
/// CEP:COST: @cold. One linear name check.
/// CEP:EVIDENCE: the writer-reuse and duplicate-section negatives in tests/test_snapshot.cpp.
Result<void> SnapshotWriter::add_section_impl(SnapshotSection kind,
                                              std::string_view name,
                                              std::vector<uint8_t> payload) {
    if (finalized_) {
        return std::unexpected(
            snap_err("writer reuse after finalize is refused"));
    }
    for (const auto& e : sections_) {
        if (e.kind == kind && e.name == name) {
            return std::unexpected(snap_err("duplicate section kind+name"));
        }
    }
    total_bytes_ += payload.size();
    sections_.push_back(
        Entry{kind, std::string(name), std::move(payload)});
    header_.section_count = static_cast<uint32_t>(sections_.size());
    return support::ok();
}

/// CEP:WHAT: Registers an anonymous section body.
/// CEP:WHY: The M0 stub surface (kept for compatibility).
/// CEP:STATUS: complete
/// CEP:FAILURE: rides through add_section_impl's named failures.
/// CEP:ASSUMES: payload is the section body (the hash prefix is prepended at finalize).
/// CEP:COST: @cold. One move + one name check.
/// CEP:EVIDENCE: small_container in tests/test_snapshot.cpp.
Result<void> SnapshotWriter::add_section(SnapshotSection kind,
                                         std::vector<uint8_t> payload) {
    return add_section_impl(kind, {}, std::move(payload));
}

/// CEP:WHAT: Registers a named section (required for multiple sections of one kind).
/// CEP:WHY: The AOT artifact names each method section 'method:<id>' (docs/infrastructure/03 section 6).
/// CEP:STATUS: complete
/// CEP:FAILURE: rides through add_section_impl's named failures.
/// CEP:ASSUMES: names are unique per kind.
/// CEP:COST: @cold. One move + one name check.
/// CEP:EVIDENCE: the AOT artifacts in tests/test_snapshot.cpp.
Result<void> SnapshotWriter::add_section_named(SnapshotSection kind,
                                               std::string_view name,
                                               std::vector<uint8_t> payload) {
    return add_section_impl(kind, name, std::move(payload));
}

/// CEP:WHAT: Registers the manifest emitted as the FIRST Metadata section.
/// CEP:WHY: The loader's manifest() reads that section; first position is the container's convention (docs/infrastructure/03 section 2).
/// CEP:STATUS: complete
/// CEP:FAILURE: named reject on reuse after finalize.
/// CEP:ASSUMES: one manifest per container.
/// CEP:COST: @cold. One payload build.
/// CEP:EVIDENCE: every manifest-bearing test in tests/test_snapshot.cpp.
Result<void> SnapshotWriter::set_manifest(const Manifest& manifest) {
    if (finalized_) {
        return std::unexpected(
            snap_err("writer reuse after finalize is refused"));
    }
    manifest_payload_ = build_manifest_payload(manifest);
    has_manifest_ = true;
    return support::ok();
}

/// CEP:WHAT: Emits the container: header, directory, name pool, hashed payloads, trailer.
/// CEP:WHY: The layout + integrity protocol is the Rule-9 contract every artifact shares (docs/infrastructure/03 section 1).
/// CEP:STATUS: complete
/// CEP:FAILURE: named reject on reuse after finalize.
/// CEP:ASSUMES: sections were registered through the add_section* surface.
/// CEP:COST: @cold. Linear in artifact bytes with two FNV passes.
/// CEP:EVIDENCE: snapshot_container_rejects_malformed (good-container reopen) in tests/test_snapshot.cpp.
Result<std::vector<uint8_t>> SnapshotWriter::finalize() {
    if (finalized_) {
        return std::unexpected(
            snap_err("writer reuse after finalize is refused"));
    }
    finalized_ = true;

    // Ordered entries: the manifest is the FIRST Metadata section.
    struct LayoutEntry {
        SnapshotSection kind;
        const std::string* name;
        const std::vector<uint8_t>* body;
    };
    std::vector<LayoutEntry> ordered;
    ordered.reserve(sections_.size() + 1);
    std::vector<uint8_t> manifest_body;
    std::string manifest_name;
    if (has_manifest_) {
        manifest_body = manifest_payload_;  // hash prefix prepended below
        manifest_name = "manifest";
        ordered.push_back(LayoutEntry{SnapshotSection::Metadata, &manifest_name,
                                      &manifest_body});
    }
    for (const auto& e : sections_) {
        ordered.push_back(LayoutEntry{e.kind, &e.name, &e.body});
    }
    header_.section_count = static_cast<uint32_t>(ordered.size());

    // Layout: header | dir | name pool | payloads (8-aligned) | trailer.
    std::vector<uint8_t> out;
    const size_t header_size = sizeof(SnapshotHeader);
    const size_t dir_size = ordered.size() * 24;
    size_t name_pool = 0;
    for (const auto& e : ordered) name_pool += e.name->size();
    size_t payload_base = header_size + dir_size + name_pool;
    payload_base = (payload_base + 7) & ~size_t{7};

    size_t cursor = payload_base;
    struct DirEntry {
        uint8_t kind;
        uint16_t name_len;
        uint32_t name_off;
        uint64_t offset;
        uint64_t size;
    };
    std::vector<DirEntry> dirs;
    dirs.reserve(ordered.size());
    std::vector<uint8_t> name_bytes;
    for (const auto& e : ordered) {
        const uint32_t name_off = static_cast<uint32_t>(name_bytes.size());
        name_bytes.insert(name_bytes.end(), e.name->begin(), e.name->end());
        dirs.push_back(DirEntry{static_cast<uint8_t>(e.kind),
                                static_cast<uint16_t>(e.name->size()),
                                name_off, cursor,
                                8 + e.body->size()});  // hash prefix + body
        cursor += 8 + e.body->size();
        cursor = (cursor + 7) & ~size_t{7};  // next payload 8-aligned
    }

    out.reserve(cursor + 16);
    // Header.
    put_u32(out, header_.magic);
    put_u16(out, header_.version_major);
    put_u16(out, header_.version_minor);
    put_u32(out, header_.section_count);
    // Directory.
    for (const auto& d : dirs) {
        out.push_back(d.kind);
        out.push_back(0);  // reserved
        put_u16(out, d.name_len);
        put_u32(out, d.name_off);
        put_u64(out, d.offset);
        put_u64(out, d.size);
    }
    // Name pool.
    out.insert(out.end(), name_bytes.begin(), name_bytes.end());
    out.resize(payload_base, 0);  // alignment pad
    // Payloads: per-payload FNV-1a hash prefix, then the body.
    const uint64_t manifest_offset = has_manifest_ ? dirs[0].offset : 0;
    for (size_t i = 0; i < ordered.size(); ++i) {
        out.resize(dirs[i].offset, 0);
        put_u64(out, fnv_bytes(*ordered[i].body));
        out.insert(out.end(), ordered[i].body->begin(),
                   ordered[i].body->end());
    }
    // Trailer: manifest offset + whole-file hash. The hash covers every
    // byte BEFORE the trailer (the spec's Rule-9 integrity domain).
    out.resize(cursor, 0);
    const uint64_t file_hash = fnv1a64(out.data(), out.size());
    put_u64(out, manifest_offset);
    put_u64(out, file_hash);
    total_bytes_ = out.size();
    return out;
}

/// CEP:WHAT: finalize() + truncate-and-write to path.
/// CEP:WHY: Artifacts are files; the AOT emitter and the tests share this one I/O path.
/// CEP:STATUS: complete
/// CEP:FAILURE: named I/O failures (open/write).
/// CEP:ASSUMES: the path is writable by the test/tool process.
/// CEP:COST: @cold. One finalize + one sequential write.
/// CEP:EVIDENCE: the AOT artifact writes in tests/test_snapshot.cpp.
Result<void> SnapshotWriter::write_file(const std::string& path) {
    auto bytes = finalize();
    if (!bytes) return std::unexpected(bytes.error());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return std::unexpected(snap_err("cannot open " + path));
    f.write(reinterpret_cast<const char*>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
    f.flush();
    if (!f) return std::unexpected(snap_err("write failed for " + path));
    return support::ok();
}

// ---- the container reader --------------------------------------------------

/// CEP:WHAT: Reads the file and runs the full Rule-9 protocol before any accessor exists.
/// CEP:WHY: A malformed artifact must never yield a live reader (Rule 9; docs/infrastructure/03 section 1).
/// CEP:STATUS: complete
/// CEP:FAILURE: named rejects in order: size, file hash, magic, version, directory bounds, section bounds, per-section hashes.
/// CEP:ASSUMES: the file is readable by the calling process.
/// CEP:COST: @cold. One read + one hash pass + one directory walk.
/// CEP:EVIDENCE: snapshot_container_rejects_malformed (all five negatives) in tests/test_snapshot.cpp.
Result<SnapshotReader> SnapshotReader::open(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::unexpected(snap_err("cannot open " + path));
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (bytes.size() < sizeof(SnapshotHeader) + 16) {
        return std::unexpected(snap_err("file too small"));
    }
    const size_t file_size = bytes.size();
    // Whole-file hash (the trailer's 8 tail bytes).
    const uint64_t stored_hash = [&] {
        uint64_t v = 0;
        std::memcpy(&v, bytes.data() + file_size - 8, 8);
        return v;
    }();
    if (fnv1a64(bytes.data(), file_size - 16) != stored_hash) {
        return std::unexpected(snap_err("file hash mismatch"));
    }
    SnapshotHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kSnapshotMagic) {
        return std::unexpected(snap_err("bad magic"));
    }
    if (header.version_major != 1 || header.version_minor != 1) {
        return std::unexpected(snap_err("version mismatch: have " +
                                        std::to_string(header.version_major) +
                                        "." +
                                        std::to_string(header.version_minor) +
                                        " want 1.1"));
    }
    const size_t dir_offset = sizeof(SnapshotHeader);
    if (dir_offset + static_cast<size_t>(header.section_count) * 24 + 16 >
        file_size) {
        return std::unexpected(snap_err("directory out of bounds"));
    }
    auto reader = SnapshotReader();
    reader.bytes_ = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    const uint8_t* base = reader.bytes_->data();
    // The name pool sits between the directory and the first payload; dir
    // entries carry POOL-RELATIVE name offsets. Names are validated in a
    // SECOND pass, against the pool region itself — the first pass's
    // per-entry payload offsets cannot bound the pool (a crafted small
    // payload offset next to a huge name_len overflowed the old check into
    // an OOB read; review 15-g-a N1 / 15-g-b F4-residual, probe-proven).
    const size_t name_pool_base = dir_offset +
                                  static_cast<size_t>(header.section_count) *
                                      24;
    uint64_t prev_end = 0;
    uint64_t pool_end = file_size;  // tightened by the smallest payload offset
    struct RawEntry {
        SnapshotSection kind;
        uint16_t name_len;
        uint32_t name_off;
        uint64_t offset;
        uint64_t size;
    };
    std::vector<RawEntry> raw;
    raw.reserve(header.section_count);
    for (uint32_t i = 0; i < header.section_count; ++i) {
        const uint8_t* d = base + dir_offset + static_cast<size_t>(i) * 24;
        RawEntry e;
        e.kind = static_cast<SnapshotSection>(d[0]);
        e.name_len =
            static_cast<uint16_t>(d[2]) | static_cast<uint16_t>(d[3]) << 8;
        std::memcpy(&e.name_off, d + 4, 4);
        std::memcpy(&e.offset, d + 8, 8);
        std::memcpy(&e.size, d + 16, 8);
        if (static_cast<uint8_t>(e.kind) > 4) {
            return std::unexpected(snap_err("unknown section kind " +
                                            std::to_string(d[0])));
        }
        if (e.offset < prev_end || e.offset % 8 != 0 ||
            e.offset > file_size - 16 ||
            e.size > file_size - 16 - e.offset) {
            // Subtractive form: the additive check wraps for near-2^64
            // offsets and would admit wild reads (review finding
            // 15-e-a/3 / 15-e-b F4, probe-proven).
            return std::unexpected(snap_err("section bounds violation"));
        }
        prev_end = e.offset + e.size;
        if (e.size < 8) {
            return std::unexpected(snap_err("section payload too small"));
        }
        if (e.offset < pool_end) pool_end = e.offset;
        raw.push_back(e);
        // Per-payload integrity: the first 8 bytes hash the rest.
        const uint64_t stored = [&] {
            uint64_t v = 0;
            std::memcpy(&v, base + e.offset, 8);
            return v;
        }();
        if (fnv1a64(base + e.offset + 8, e.size - 8) != stored) {
            return std::unexpected(snap_err("section hash mismatch"));
        }
    }
    // Second pass: names live inside [name_pool_base, pool_end).
    for (uint32_t i = 0; i < header.section_count; ++i) {
        const RawEntry& e = raw[i];
        Entry entry;
        entry.kind = e.kind;
        entry.offset = e.offset;
        entry.size = e.size;
        if (e.name_len > 0) {
            const uint64_t name_begin = name_pool_base + e.name_off;
            if (name_begin < name_pool_base ||
                name_begin > pool_end ||
                static_cast<uint64_t>(e.name_len) > pool_end - name_begin) {
                return std::unexpected(snap_err("section name out of bounds"));
            }
            entry.name.assign(
                reinterpret_cast<const char*>(base + name_begin), e.name_len);
        }
        reader.entries_.push_back(std::move(entry));
    }
    return reader;
}

/// CEP:WHAT: Number of verified sections.
/// CEP:WHY: The M0 stub surface (kept).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure query over verified state).
/// CEP:ASSUMES: called on a reader returned by open().
/// CEP:COST: @cold. One size read.
/// CEP:EVIDENCE: snapshot_container_rejects_malformed's good-container check.
Result<size_t> SnapshotReader::section_count() const {
    return static_cast<size_t>(entries_.size());
}

/// CEP:WHAT: The first matching section's body (past the hash prefix).
/// CEP:WHY: Loaders slice the container through this one accessor; the
///           empty-name form matches the first section of the kind.
/// CEP:STATUS: complete
/// CEP:FAILURE: named reject when no section matches.
/// CEP:ASSUMES: called on a verified reader.
/// CEP:COST: @cold. One directory scan.
/// CEP:EVIDENCE: the heap/code/AOT loads in tests/test_snapshot.cpp.
Result<std::span<const uint8_t>> SnapshotReader::section_payload(
    SnapshotSection kind, std::string_view name) const {
    for (const auto& e : entries_) {
        if (e.kind != kind) continue;
        if (!name.empty() && e.name != name) continue;
        const uint8_t* base = bytes_->data();
        return std::span<const uint8_t>(base + e.offset + 8,
                                        static_cast<size_t>(e.size - 8));
    }
    return std::unexpected(snap_err("no such section"));
}

/// CEP:WHAT: Parses the FIRST Metadata section as the manifest.
/// CEP:WHY: The container's manifest convention (docs/infrastructure/03 section 2).
/// CEP:STATUS: complete
/// CEP:FAILURE: named reject when the container has no manifest or the payload is malformed.
/// CEP:ASSUMES: called on a verified reader.
/// CEP:COST: @cold. One scan + one parse.
/// CEP:EVIDENCE: the manifest round trips in tests/test_snapshot.cpp.
Result<Manifest> SnapshotReader::manifest() const {
    for (const auto& e : entries_) {
        if (e.kind != SnapshotSection::Metadata) continue;
        const uint8_t* base = bytes_->data();
        return parse_manifest_payload(
            std::span<const uint8_t>(base + e.offset + 8, e.size - 8));
    }
    return std::unexpected(snap_err("container has no manifest"));
}

// ---- section names ---------------------------------------------------------

/// CEP:WHAT: Names of the sections of one kind, in file order.
/// CEP:WHY: The AOT loader discovers its per-method MachineCode sections through this (docs/infrastructure/03 section 6).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure query; an empty list is a valid answer).
/// CEP:ASSUMES: called on a verified reader.
/// CEP:COST: @cold. One pass over the directory.
/// CEP:EVIDENCE: aot_pipeline_deterministic's hot-section check in tests/test_snapshot.cpp.
std::vector<std::string> SnapshotReader::section_names(
    SnapshotSection kind) const {
    std::vector<std::string> names;
    for (const auto& e : entries_) {
        if (e.kind == kind) names.push_back(e.name);
    }
    return names;
}

// ---- heap snapshots ---------------------------------------------------------

namespace {

/// Appends a tagged word, swizzling live heap references to
/// (0xFFFF'FFFF << 32) | object index.
/// CEP:WHAT: Appends a tagged word, swizzling live heap references to (0xFFFF'FFFF << 32) | index.
/// CEP:WHY: The swizzle is the heap snapshot's relocation form (docs/infrastructure/03 section 3).
/// CEP:STATUS: complete
/// CEP:FAILURE: named reject when a heap word points outside the captured object set.
/// CEP:ASSUMES: obj_index covers every captured object.
/// CEP:COST: @cold. One tag test + one hash lookup per word.
/// CEP:EVIDENCE: heap_snapshot_roundtrip_parity in tests/test_snapshot.cpp.
Result<void> emit_word(std::vector<uint8_t>& out, const TaggedValue& word,
                       const FlatHashMap<const void*, uint32_t>& obj_index) {
    const uint64_t bits = word.raw();
    if (is_heap_word(bits)) {
        const void* p = reinterpret_cast<const void*>(bits & ~uint64_t{0xF});
        const uint32_t* idx = obj_index.find(p);
        if (idx == nullptr) {
            return std::unexpected(
                snap_err("object references a foreign heap address"));
        }
        put_u64(out, (kSwizzleHi << 32) | kSwizzleTag |
                         (static_cast<uint64_t>(*idx) << 4));
        return support::ok();
    }
    put_u64(out, bits);
    return support::ok();
}

/// CEP:WHAT: Reverses the swizzle: index -> restored address, tag reapplied.
/// CEP:WHY: The restore pass pairs with emit_word (docs/infrastructure/03 section 3).
/// CEP:STATUS: complete
/// CEP:FAILURE: none here; the caller validates the index range.
/// CEP:ASSUMES: objects is the fully allocated restore table.
/// CEP:COST: @cold. One tag test + one vector index.
/// CEP:EVIDENCE: heap_snapshot_roundtrip_parity in tests/test_snapshot.cpp.
TaggedValue rewrite_word(uint64_t bits,
                         const std::vector<HeapObject*>& objects) {
    if (is_swizzled_word(bits)) {
        // Indices are validated in the parse pass (Rule 9); the rewrite is
        // infallible by construction.
        HeapObject* obj = objects[swizzle_index(bits)];
        return TaggedValue::from_raw(reinterpret_cast<uint64_t>(obj) |
                                     kHeapTagBits);
    }
    return TaggedValue::from_raw(bits);
}

}  // namespace

/// CEP:WHAT: Serializes the logical heap: the klass table (Shapes) + the object graph (Objects) with pointer swizzling.
/// CEP:WHY: Startup acceleration without core dumps: GC runtime state is excluded by contract (docs/infrastructure/03 section 3).
/// CEP:STATUS: complete
/// CEP:FAILURE: named rejects on foreign klass pointers, torn chains, and index-map failures.
/// CEP:ASSUMES: the heap is quiescent (Rule 24) and its object chains are intact.
/// CEP:COST: @cold. O(objects): one enumeration pass, one serialization pass.
/// CEP:EVIDENCE: heap_snapshot_roundtrip_parity + heap_snapshot_restore_deterministic in tests/test_snapshot.cpp.
Result<void> write_heap_snapshot(gc::Heap& heap, KlassRegistry& klasses,
                                 SnapshotWriter& out) {
    // ---- klass table -------------------------------------------------------
    std::vector<Klass*> klass_list;
    FlatHashMap<const void*, uint32_t> klass_index;
    klass_list.reserve(klasses.size() + 1);
    for (size_t i = 0; i < klasses.size(); ++i) {
        Klass* k = klasses.at(i);
        if (klass_index.insert(k, static_cast<uint32_t>(klass_list.size())) ==
            nullptr) {
            return std::unexpected(snap_err("klass index insert failed"));
        }
        klass_list.push_back(k);
    }
    Klass* dk = heap.double_klass();
    if (dk != nullptr && klass_index.find(dk) == nullptr) {
        if (klass_index.insert(dk, static_cast<uint32_t>(klass_list.size())) ==
            nullptr) {
            return std::unexpected(snap_err("klass index insert failed"));
        }
        klass_list.push_back(dk);
    }

    std::vector<uint8_t> shapes;
    put_u32(shapes, static_cast<uint32_t>(klass_list.size()));
    for (Klass* k : klass_list) {
        const bool is_double = (dk != nullptr && k == dk);
        shapes.push_back(is_double ? 1 : 0);
        put_str(shapes, k->name());
        put_u32(shapes, k->field_count());
        for (const auto& f : k->fields()) {
            put_str(shapes, f.name);
            put_u32(shapes, f.offset);
        }
        put_u32(shapes, static_cast<uint32_t>(k->methods().size()));
        for (const auto& m : k->methods()) {
            put_str(shapes, m.name);
            shapes.push_back(static_cast<uint8_t>(m.kind));
            put_u32(shapes, m.ugb_method_id);
        }
    }
    auto st = out.add_section_named(SnapshotSection::Shapes, "heap-klasses",
                                    std::move(shapes));
    if (!st) return st;

    // ---- object enumeration (young chain, then each old region in order) ----
    std::vector<HeapObject*> objects;
    FlatHashMap<const void*, uint32_t> obj_index;
    const auto record = [&](HeapObject* o) -> Result<void> {
        if (obj_index.find(o) != nullptr) {
            return std::unexpected(
                snap_err("heap walk visited an object twice"));
        }
        if (obj_index.insert(o, static_cast<uint32_t>(objects.size())) ==
            nullptr) {
            return std::unexpected(snap_err("object index insert failed"));
        }
        objects.push_back(o);
        return support::ok();
    };
    {
        // Adopt the bound JIT context's bump position first (the same
        // protocol collect_young uses): the stencils bump the CONTEXT copy
        // between slow paths, and a stale top would drop JIT-allocated
        // young objects (review finding 15-e-a/4).
        heap.sync_tlab_from_jit();
        uint8_t* top = heap.tlab_top();
        HeapObject* o = reinterpret_cast<HeapObject*>(heap.young_base());
        while (reinterpret_cast<uint8_t*>(o) < top) {
            auto r = record(o);
            if (!r) return r;
            if (o->header.size == 0) return std::unexpected(
                snap_err("torn young chain (zero-size header)"));
            o = reinterpret_cast<HeapObject*>(
                reinterpret_cast<uint8_t*>(o) + o->header.size);
        }
    }
    for (const auto& r : heap.regions()) {
        if (r.state == gc::RegionState::Free) continue;
        if (r.state == gc::RegionState::HumongousCont) continue;
        if (r.state == gc::RegionState::HumongousStart) {
            // One object spans the whole run; its header size covers it.
            auto* o = reinterpret_cast<HeapObject*>(r.start);
            auto rec = record(o);
            if (!rec) return rec;
            continue;
        }
        const uint8_t* end =
            r.top != nullptr ? r.top : r.start + gc::Region::kSize;
        HeapObject* o = reinterpret_cast<HeapObject*>(r.start);
        while (reinterpret_cast<uint8_t*>(o) < end) {
            auto rec = record(o);
            if (!rec) return rec;
            if (o->header.size == 0) return std::unexpected(
                snap_err("torn old chain (zero-size header)"));
            o = reinterpret_cast<HeapObject*>(
                reinterpret_cast<uint8_t*>(o) + o->header.size);
        }
    }

    // ---- object records ----------------------------------------------------
    std::vector<uint8_t> objs;
    put_u32(objs, static_cast<uint32_t>(objects.size()));
    for (HeapObject* o : objects) {
        const uint64_t bits = reinterpret_cast<uint64_t>(o);
        if (o->header.klass == nullptr) {
            // Array (the M0 array type marker: klass == nullptr).
            auto* arr = reinterpret_cast<ArrayObject*>(bits);
            objs.push_back(1);
            put_u32(objs, 0);  // klass_index unused for arrays
            put_u32(objs, arr->length());
            for (uint32_t i = 0; i < arr->length(); ++i) {
                auto w = emit_word(objs, arr->element(i), obj_index);
                if (!w) return std::unexpected(w.error());
            }
            continue;
        }
        const bool is_double =
            dk != nullptr && o->header.klass == dk;
        if (is_double) {
            objs.push_back(2);
            put_u32(objs, 0);
            const double v = read_boxed_double(o);
            uint8_t payload[8];
            std::memcpy(payload, &v, 8);
            objs.insert(objs.end(), payload, payload + 8);
            continue;
        }
        const auto kit = klass_index.find(o->header.klass);
        if (kit == nullptr) {
            return std::unexpected(
                snap_err("object's klass is outside the registry"));
        }
        Klass* k = klass_list[*kit];
        // The KLASS owns the field count (the authoritative layout); the
        // header size may add alignment pad beyond the last field. Pad
        // words are zeroed at allocation (icggc.cpp) so they serialize as
        // nulls, and they are NOT serialized as fields (review finding
        // 15-e-a/5: stale pad bytes after a GC would be ghost references).
        const uint32_t field_count = k->field_count();
        const uint32_t size_fields = static_cast<uint32_t>(
            (o->header.size - sizeof(ObjectHeader)) / sizeof(TaggedValue));
        if (size_fields < field_count) {
            return std::unexpected(
                snap_err("object size disagrees with its klass layout"));
        }
        objs.push_back(0);
        put_u32(objs, *kit);
        put_u32(objs, field_count);
        auto* obj = reinterpret_cast<Object*>(bits);
        for (uint32_t i = 0; i < field_count; ++i) {
            auto w = emit_word(objs, obj->field(i), obj_index);
            if (!w) return std::unexpected(w.error());
        }
    }
    return out.add_section_named(SnapshotSection::Objects, "heap-objects",
                                 std::move(objs));
}

/// CEP:WHAT: Rebuilds the heap: klasses first, then objects in snapshot order, then the pointer-word rewrite.
/// CEP:WHY: Allocation order is snapshot data, so the layout is offset-deterministic (the section-3 contract).
/// CEP:STATUS: complete
/// CEP:FAILURE: named rejects on every malformed record, range violation, and allocation failure; no partial state escapes.
/// CEP:ASSUMES: the reader passed the container's Rule-9 protocol.
/// CEP:COST: @cold. O(objects): parse, allocate, rewrite.
/// CEP:EVIDENCE: heap_snapshot_roundtrip_parity + heap_snapshot_restore_deterministic in tests/test_snapshot.cpp.
Result<RestoredHeap> restore_heap_snapshot(const SnapshotReader& in,
                                           const HeapRestoreConfig& cfg) {
    auto shapes = in.section_payload(SnapshotSection::Shapes, "heap-klasses");
    if (!shapes) return std::unexpected(shapes.error());
    auto objs = in.section_payload(SnapshotSection::Objects, "heap-objects");
    if (!objs) return std::unexpected(objs.error());

    Cursor sc(*shapes);
    const uint32_t klass_count = sc.u32();
    if (!sc.ok || klass_count > 65536) {
        return std::unexpected(snap_err("klass table header invalid"));
    }
    std::vector<Klass*> klass_by_index;

    RestoredHeap out;
    out.heap = std::make_unique<gc::Heap>(cfg.young_bytes, cfg.old_bytes);
    out.klasses = std::make_unique<KlassRegistry>();
    for (uint32_t i = 0; i < klass_count; ++i) {
        const uint8_t is_double = sc.u8();
        std::string name = sc.str();
        const uint32_t field_count = sc.u32();
        if (!sc.ok || field_count > 65536) {
            return std::unexpected(snap_err("klass record invalid"));
        }
        Klass* k = nullptr;
        if (is_double) {
            k = out.heap->double_klass();
            if (k == nullptr) {
                return std::unexpected(snap_err("heap has no double klass"));
            }
            // The double klass's field/method records are consumed but
            // discarded (the fresh heap's static klass owns its own layout
            // — desyncing the cursor here would corrupt every later read,
            // review finding 15-e-a/6).
            for (uint32_t f = 0; f < field_count; ++f) {
                (void)sc.str();
                (void)sc.u32();
            }
        } else {
            k = out.klasses->create(name);
            for (uint32_t f = 0; f < field_count; ++f) {
                std::string fname = sc.str();
                const uint32_t recorded_offset = sc.u32();
                k->add_field(fname);
                const uint32_t built = k->fields().back().offset;
                if (built != recorded_offset) {
                    return std::unexpected(snap_err(
                        "klass field offset drift on " + name + "." + fname +
                        ": recorded " + std::to_string(recorded_offset) +
                        " built " + std::to_string(built)));
                }
            }
        }
        const uint32_t method_count = sc.u32();
        if (!sc.ok || method_count > 65536) {
            return std::unexpected(snap_err("klass record invalid"));
        }
        if (!is_double) {
            for (uint32_t m = 0; m < method_count; ++m) {
                MethodEntry entry;
                entry.name = sc.str();
                entry.kind = static_cast<MethodImplementationKind>(sc.u8());
                entry.ugb_method_id = sc.u32();
                k->methods().push_back(std::move(entry));
            }
        } else {
            // Skip the double klass's recorded methods (the fresh heap's
            // klass owns its own method set).
            for (uint32_t m = 0; m < method_count; ++m) {
                (void)sc.str();
                (void)sc.u8();
                (void)sc.u32();
            }
        }
        if (!sc.ok) {
            return std::unexpected(snap_err("klass record truncated"));
        }
        klass_by_index.push_back(k);
        if (is_double) out.double_klass = k;
    }

    Cursor oc(*objs);
    const uint32_t object_count = oc.u32();
    if (!oc.ok || object_count > 1'000'000) {
        return std::unexpected(snap_err("object table header invalid"));
    }
    struct RawObject {
        uint8_t kind;
        uint32_t klass_index;
        uint32_t count;        // field_count (plain) / length (array)
        std::vector<uint64_t> words;
        double value = 0.0;    // boxed double
    };
    std::vector<RawObject> raw;
    raw.reserve(object_count);
    for (uint32_t i = 0; i < object_count; ++i) {
        RawObject r;
        r.kind = oc.u8();
        r.klass_index = oc.u32();
        if (!oc.ok || r.klass_index >= klass_by_index.size()) {
            return std::unexpected(snap_err("object klass index invalid"));
        }
        if (r.kind == 0) {
            r.count = oc.u32();
            if (!oc.ok || r.count > 65536) {
                return std::unexpected(snap_err("object field count invalid"));
            }
            r.words.resize(r.count);
            for (auto& w : r.words) {
                w = oc.u64();
                // Rule 9: validate swizzle targets BEFORE anything is
                // allocated — a crafted index can never reach the rewrite
                // (review finding 15-e-a/2: the old .at() path died on an
                // unhandled std::out_of_range for malformed input).
                if (is_swizzled_word(w) &&
                    swizzle_index(w) >= object_count) {
                    return std::unexpected(
                        snap_err("object word swizzle index out of range"));
                }
            }
        } else if (r.kind == 1) {
            r.count = oc.u32();
            if (!oc.ok || r.count > 1'000'000) {
                return std::unexpected(snap_err("array length invalid"));
            }
            r.words.resize(r.count);
            for (auto& w : r.words) {
                w = oc.u64();
                if (is_swizzled_word(w) &&
                    swizzle_index(w) >= object_count) {
                    return std::unexpected(
                        snap_err("array word swizzle index out of range"));
                }
            }
        } else if (r.kind == 2) {
            uint8_t payload[8] = {0};
            oc.take(payload, 8);
            std::memcpy(&r.value, payload, 8);
        } else {
            return std::unexpected(snap_err("unknown object kind"));
        }
        if (!oc.ok) return std::unexpected(snap_err("object record truncated"));
        raw.push_back(std::move(r));
    }

    // Allocation pass (snapshot order -> deterministic layout).
    out.objects.resize(object_count, nullptr);
    for (uint32_t i = 0; i < object_count; ++i) {
        const auto& r = raw[i];
        if (r.kind == 0) {
            auto a = out.heap->allocate_object(klass_by_index[r.klass_index],
                                               r.count);
            if (!a) return std::unexpected(a.error());
            out.objects[i] = *a;
        } else if (r.kind == 1) {
            auto a = out.heap->allocate_array(r.count);
            if (!a) return std::unexpected(a.error());
            out.objects[i] = *a;
        } else {
            auto a = out.heap->allocate_double(r.value);
            if (!a) return std::unexpected(a.error());
            out.objects[i] = *a;
        }
    }

    // Pointer-word rewrite pass.
    for (uint32_t i = 0; i < object_count; ++i) {
        const auto& r = raw[i];
        if (r.kind == 0) {
            auto* obj = static_cast<Object*>(out.objects[i]);
            for (uint32_t f = 0; f < r.count; ++f) {
                obj->field(f) = rewrite_word(r.words[f], out.objects);
            }
        } else if (r.kind == 1) {
            auto* arr = static_cast<ArrayObject*>(out.objects[i]);
            for (uint32_t e = 0; e < r.count; ++e) {
                arr->element(e) = rewrite_word(r.words[e], out.objects);
            }
        }
    }
    return out;
}

/// CEP:WHAT: The name->address klass table the code cache verifies against.
/// CEP:WHY: The loader checks every baked handle against the loading runtime (docs/infrastructure/03 section 4).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure collector).
/// CEP:ASSUMES: klass names are the identity the artifact records.
/// CEP:COST: @cold. O(klasses).
/// CEP:EVIDENCE: code_cache_reload_parity + the AOT artifacts in tests/test_snapshot.cpp.
std::vector<KlassHandle> heap_klass_handles(KlassRegistry& klasses,
                                            gc::Heap& heap) {
    std::vector<KlassHandle> handles;
    handles.reserve(klasses.size() + 1);
    Klass* dk = heap.double_klass();
    for (size_t i = 0; i < klasses.size(); ++i) {
        Klass* k = klasses.at(i);
        if (dk != nullptr && k == dk) continue;  // listed once, below
        handles.push_back(KlassHandle{k->name(), k});
    }
    if (dk != nullptr) {
        handles.push_back(KlassHandle{dk->name(), dk});
    }
    return handles;
}

// ---- the shared code cache -------------------------------------------------

namespace {

/// CEP:WHAT: Serializes one deopt frame including its rematerialization descriptors.
/// CEP:WHY: The stubs reference the LIVE records (remats included), so the code snapshot carries them (docs/infrastructure/03 section 4).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure serialization).
/// CEP:ASSUMES: f is a compiled frame state.
/// CEP:COST: @cold. Linear in the frame.
/// CEP:EVIDENCE: code_cache_reload_parity's record-count check in tests/test_snapshot.cpp.
void put_deopt_frame(std::vector<uint8_t>& out, const j2::DeoptFrame& f) {
    put_u32(out, f.method_id);
    put_u32(out, f.resume_pc);
    put_u32(out, f.inject_dst);
    put_u32(out, f.vreg_base);
    put_u32(out, f.vreg_count);
    put_u32(out, static_cast<uint32_t>(f.remats.size()));
    for (const auto& r : f.remats) {
        put_u32(out, r.slot);
        put_u32(out, r.klass_token);
        put_u32(out, r.field_count);
        // Each vector carries its OWN count: const_words runs parallel to
        // the CONST fields only (the emitter appends one word per Const
        // field), so its size can differ from field_keys' (review finding
        // 15-e-b F1, probe-proven).
        put_u32(out, static_cast<uint32_t>(r.field_keys.size()));
        for (uint32_t k : r.field_keys) put_u32(out, k);
        put_u32(out, static_cast<uint32_t>(r.field_slots.size()));
        for (uint32_t s : r.field_slots) put_u32(out, s);
        put_u32(out, static_cast<uint32_t>(r.const_words.size()));
        for (uint64_t w : r.const_words) put_u64(out, w);
    }
}

/// CEP:WHAT: Parses one deopt frame with full bounds checking.
/// CEP:WHY: Rule 9: untrusted frame payloads are range-checked before use.
/// CEP:STATUS: complete
/// CEP:FAILURE: a poisoned cursor; the caller rejects.
/// CEP:ASSUMES: the caller checks cursor state after the call.
/// CEP:COST: @cold. Linear in the frame.
/// CEP:EVIDENCE: code_cache_reload_parity in tests/test_snapshot.cpp.
Result<j2::DeoptFrame> take_deopt_frame(Cursor& c) {
    j2::DeoptFrame f;
    f.method_id = c.u32();
    f.resume_pc = c.u32();
    f.inject_dst = c.u32();
    f.vreg_base = c.u32();
    f.vreg_count = c.u32();
    const uint32_t remat_count = c.u32();
    if (!c.ok || remat_count > 4096) {
        c.ok = false;  // poison: oversized counts must reject, not pass
        return f;
    }
    f.remats.resize(remat_count);
    for (auto& r : f.remats) {
        r.slot = c.u32();
        r.klass_token = c.u32();
        r.field_count = c.u32();
        const uint32_t key_count = c.u32();
        if (!c.ok || key_count > 4096) {
            c.ok = false;
            return f;
        }
        r.field_keys.resize(key_count);
        for (auto& k : r.field_keys) k = c.u32();
        const uint32_t slot_count = c.u32();
        if (!c.ok || slot_count > 4096) {
            c.ok = false;
            return f;
        }
        r.field_slots.resize(slot_count);
        for (auto& s : r.field_slots) s = c.u32();
        if (slot_count != key_count) {
            // field_keys and field_slots are parallel by contract (the
            // consumer indexes field_keys[i] per slot i); a desynced pair
            // would OOB the consumer (review 15-g-b N2, probe-proven).
            c.ok = false;
            return f;
        }
        const uint32_t word_count = c.u32();
        if (!c.ok || word_count > 4096) {
            c.ok = false;
            return f;
        }
        r.const_words.resize(word_count);
        for (auto& w : r.const_words) w = c.u64();
        // The consumer walks fields and consumes const words in emitter
        // order; the invariant it needs: one word per kConstSlot entry.
        uint32_t const_entries = 0;
        for (uint32_t s : r.field_slots) {
            if (s == j2::DeoptFrame::RematEntry::kConstSlot) ++const_entries;
        }
        if (const_entries != r.const_words.size()) {
            c.ok = false;
            return f;
        }
    }
    return f;
}

/// Serializes/loads one MachineCode payload (shared by the single-method
/// code cache and the AOT artifact's per-method sections).
/// CEP:WHAT: Serializes one compiled method: code bytes, offsets, GC maps, live records, region table, relocs, summary, handle table.
/// CEP:WHY: The shared code cache and the AOT artifact use the SAME MachineCode payload (docs/infrastructure/03 sections 4/6).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure serialization).
/// CEP:ASSUMES: code is a completed compile; handles is the compile-time handle set.
/// CEP:COST: @cold. Linear in the method's metadata.
/// CEP:EVIDENCE: code_cache_reload_parity in tests/test_snapshot.cpp.
std::vector<uint8_t> build_code_payload(
    const j2::J2Code& code, const std::vector<KlassHandle>& handles) {
    std::vector<uint8_t> p;
    put_u32(p, code.method_id);
    put_u32(p, code.entry_offset);
    put_u32(p, code.osr_entry_offset);
    put_u32(p, static_cast<uint32_t>(code.osr_pcs.size()));
    for (uint32_t pc : code.osr_pcs) put_u32(p, pc);
    put_u32(p, static_cast<uint32_t>(code.code.size()));
    p.insert(p.end(), code.code.begin(), code.code.end());
    put_u32(p, static_cast<uint32_t>(code.gc_maps.size()));
    p.insert(p.end(), code.gc_maps.begin(), code.gc_maps.end());
    put_u32(p, static_cast<uint32_t>(code.deopt_records.size()));
    p.insert(p.end(), code.deopt_records.begin(), code.deopt_records.end());
    // Region table (fresh counters at load are runtime state).
    put_u32(p, static_cast<uint32_t>(code.regions.size()));
    for (const auto& r : code.regions.regions()) {
        put_u32(p, r.region_id);
        p.push_back(static_cast<uint8_t>(r.kind));
        p.push_back(static_cast<uint8_t>(r.state));
        put_u32(p, r.entry_offset);
        put_u32(p, r.exit_offset);
        put_u32(p, r.bytecode_pc_begin);
        put_u32(p, r.bytecode_pc_end);
        p.push_back(r.tier);
        put_u32(p, 0);  // failure_count resets at load
        put_u32(p, r.deopt.region_id);
        put_u32(p, r.deopt.bytecode_resume_pc);
        put_u32(p, r.deopt.successor_region_id);
        p.push_back(r.deopt.tier_fallback_target);
        put_u32(p, static_cast<uint32_t>(r.deopt.escape_locations.size()));
        for (const auto& e : r.deopt.escape_locations) {
            put_u16(p, e.virtual_register);
            p.push_back(static_cast<uint8_t>(e.location));
            put_u32(p, e.payload);
        }
    }
    // Live deopt records (frames + remats — the stubs reference THESE).
    const uint32_t record_count =
        code.records ? static_cast<uint32_t>(code.records->size()) : 0;
    put_u32(p, record_count);
    for (uint32_t i = 0; i < record_count; ++i) {
        const auto& rec = (*code.records)[i];
        put_u32(p, rec.region_id);
        p.push_back(rec.suspension ? 1 : 0);
        put_u32(p, static_cast<uint32_t>(rec.frames.size()));
        for (const auto& f : rec.frames) put_deopt_frame(p, f);
    }
    put_u32(p, static_cast<uint32_t>(code.record_relocs.size()));
    for (const auto& r : code.record_relocs) {
        put_u32(p, r.code_offset);
        put_u32(p, r.record_index);
    }
    p.push_back(code.has_summary ? 1 : 0);
    if (code.has_summary) {
        put_u32(p, code.summary.method_id);
        put_u32(p, code.summary.param_count);
        put_u32(p, static_cast<uint32_t>(code.summary.params.size()));
        for (const auto& e : code.summary.params) {
            p.push_back(static_cast<uint8_t>(e));
        }
        p.push_back(code.summary.returns_heap_allocation ? 1 : 0);
        p.push_back(code.summary.captures_global ? 1 : 0);
        put_u64(p, code.summary.graph_hash);
    }
    p.push_back(code.budget_exceeded ? 1 : 0);
    put_u32(p, static_cast<uint32_t>(handles.size()));
    for (const auto& h : handles) {
        put_str(p, h.name);
        put_u64(p, reinterpret_cast<uint64_t>(h.address));
    }
    return p;
}

/// CEP:WHAT: Rebuilds a J2Code from a MachineCode payload; verifies every klass handle by name AND address.
/// CEP:WHY: Rule 98 dependency validity: a drifted handle means the artifact was compiled against a different runtime (docs/infrastructure/03 section 4).
/// CEP:STATUS: complete
/// CEP:FAILURE: named rejects on structure violations, range violations, and handle drift/absence.
/// CEP:ASSUMES: current is the loading runtime's handle table.
/// CEP:COST: @cold. Linear in the payload.
/// CEP:EVIDENCE: code_cache_reload_parity (parity + drift reject) in tests/test_snapshot.cpp.
Result<j2::J2Code> take_code_payload(std::span<const uint8_t> body,
                                     const std::vector<KlassHandle>& current) {
    Cursor c(body);
    j2::J2Code code;
    code.method_id = c.u32();
    code.entry_offset = c.u32();
    code.osr_entry_offset = c.u32();
    // Entry points index the code buffer; anything else would hand the
    // publisher a wild function pointer (review finding 15-e-b F5).
    // Offsets are validated again once code_size is known (below).
    if (!c.ok) {
        return std::unexpected(snap_err("code payload header invalid"));
    }
    const uint32_t osr_pc_count = c.u32();
    if (!c.ok || osr_pc_count > 65536) {
        return std::unexpected(snap_err("code payload header invalid"));
    }
    code.osr_pcs.resize(osr_pc_count);
    for (auto& pc : code.osr_pcs) pc = c.u32();
    const uint32_t code_size = c.u32();
    if (!c.ok || code_size > (1u << 26)) {
        return std::unexpected(snap_err("code size invalid"));
    }
    code.code.resize(code_size);
    c.take(code.code.data(), code_size);
    if (!c.ok || code.entry_offset >= code_size ||
        (code.osr_entry_offset != 0xFFFFFFFFu &&
         code.osr_entry_offset >= code_size)) {
        return std::unexpected(snap_err("entry offset out of range"));
    }
    const uint32_t gc_maps_size = c.u32();
    if (!c.ok || gc_maps_size > (1u << 26)) {
        return std::unexpected(snap_err("gc maps size invalid"));
    }
    code.gc_maps.resize(gc_maps_size);
    c.take(code.gc_maps.data(), gc_maps_size);
    const uint32_t records_blob_size = c.u32();
    if (!c.ok || records_blob_size > (1u << 26)) {
        return std::unexpected(snap_err("deopt records size invalid"));
    }
    code.deopt_records.resize(records_blob_size);
    c.take(code.deopt_records.data(), records_blob_size);

    const uint32_t region_count = c.u32();
    if (!c.ok || region_count > 65536) {
        return std::unexpected(snap_err("region table size invalid"));
    }
    for (uint32_t i = 0; i < region_count; ++i) {
        deopt::RegionDescriptor d;
        d.region_id = c.u32();
        d.kind = static_cast<deopt::RegionKind>(c.u8());
        d.state = static_cast<deopt::RegionState>(c.u8());
        d.entry_offset = c.u32();
        d.exit_offset = c.u32();
        d.bytecode_pc_begin = c.u32();
        d.bytecode_pc_end = c.u32();
        d.tier = c.u8();
        (void)c.u32();  // failure_count is written but resets at load
        d.deopt.region_id = c.u32();
        d.deopt.bytecode_resume_pc = c.u32();
        d.deopt.successor_region_id = c.u32();
        d.deopt.tier_fallback_target = c.u8();
        const uint32_t escape_count = c.u32();
        if (!c.ok || escape_count > 4096) {
            return std::unexpected(snap_err("region escape set invalid"));
        }
        d.deopt.escape_locations.resize(escape_count);
        for (auto& e : d.deopt.escape_locations) {
            e.virtual_register = c.u16();
            e.location = static_cast<deopt::EscapeLocation::Loc>(c.u8());
            e.payload = c.u32();
        }
        if (!c.ok) return std::unexpected(snap_err("region record truncated"));
        code.regions.add_region(std::move(d));
    }

    const uint32_t record_count = c.u32();
    if (!c.ok || record_count > 65536) {
        return std::unexpected(snap_err("record table size invalid"));
    }
    auto records = std::make_shared<std::vector<j2::DeoptRecord>>();
    records->resize(record_count);
    for (uint32_t i = 0; i < record_count; ++i) {
        auto& rec = (*records)[i];
        rec.region_id = c.u32();
        rec.suspension = c.u8() != 0;
        const uint32_t frame_count = c.u32();
        if (!c.ok || frame_count > j2::kMaxDeoptFrames) {
            return std::unexpected(snap_err("record frame count invalid"));
        }
        rec.frames.resize(frame_count);
        for (auto& f : rec.frames) {
            auto fr = take_deopt_frame(c);
            if (!c.ok) return std::unexpected(snap_err("frame truncated"));
            f = std::move(*fr);
        }
    }
    code.records = std::move(records);

    const uint32_t reloc_count = c.u32();
    if (!c.ok || reloc_count > 65536) {
        return std::unexpected(snap_err("reloc table size invalid"));
    }
    code.record_relocs.resize(reloc_count);
    for (auto& r : code.record_relocs) {
        r.code_offset = c.u32();
        r.record_index = c.u32();
        // Rule 9 + Rule 76: a wild reloc would bake a wild pointer into
        // executable memory at publish time (review finding 15-e-b F5,
        // probe-proven) — validate against the payload's own tables, in
        // u64 (the u32 add wrapped at 0xFFFFFFF8 and re-admitted the
        // wild write; review 15-g-b N1).
        if (!c.ok ||
            static_cast<uint64_t>(r.code_offset) + 8 > code.code.size() ||
            r.record_index >= code.records->size()) {
            return std::unexpected(snap_err("record reloc out of range"));
        }
    }
    code.has_summary = c.u8() != 0;
    if (code.has_summary) {
        code.summary.method_id = c.u32();
        code.summary.param_count = c.u32();
        const uint32_t param_n = c.u32();
        if (!c.ok || param_n > 4096) {
            return std::unexpected(snap_err("summary param count invalid"));
        }
        code.summary.params.resize(param_n);
        for (auto& e : code.summary.params) {
            e = static_cast<ir::ParamEscape>(c.u8());
        }
        code.summary.returns_heap_allocation = c.u8() != 0;
        code.summary.captures_global = c.u8() != 0;
        code.summary.graph_hash = c.u64();
    }
    code.budget_exceeded = c.u8() != 0;

    const uint32_t handle_count = c.u32();
    if (!c.ok || handle_count > 65536) {
        return std::unexpected(snap_err("handle table size invalid"));
    }
    for (uint32_t i = 0; i < handle_count; ++i) {
        const std::string name = c.str();
        uint64_t baked = 0;
        {
            uint8_t b[8] = {0};
            c.take(b, 8);
            std::memcpy(&baked, b, 8);
        }
        if (!c.ok) return std::unexpected(snap_err("handle table truncated"));
        // Rule 98 dependency validity: every baked klass handle must match
        // the loading runtime by name AND address (the restore-determinism
        // contract makes them equal).
        bool matched = false;
        for (const auto& h : current) {
            if (h.name == name) {
                if (reinterpret_cast<uint64_t>(h.address) != baked) {
                    return std::unexpected(snap_err(
                        "klass handle mismatch on " + name));
                }
                matched = true;
                break;
            }
        }
        if (!matched) {
            return std::unexpected(snap_err("klass handle missing: " + name));
        }
    }
    if (!c.ok || c.remaining() != 0) {
        return std::unexpected(snap_err("code payload truncated"));
    }
    return code;
}

}  // namespace

/// CEP:WHAT: Registers one MachineCode section named 'method:<id>'.
/// CEP:WHY: Named sections let the AOT artifact carry several methods in one container.
/// CEP:STATUS: complete
/// CEP:FAILURE: rides through add_section_impl's named failures.
/// CEP:ASSUMES: one code snapshot per method per container (name uniqueness).
/// CEP:COST: @cold. One payload build.
/// CEP:EVIDENCE: code_cache_reload_parity in tests/test_snapshot.cpp.
Result<void> write_code_snapshot(const j2::J2Code& code,
                                 const std::vector<KlassHandle>& handles,
                                 SnapshotWriter& out) {
    return out.add_section_named(
        SnapshotSection::MachineCode, "method:" + std::to_string(code.method_id),
        build_code_payload(code, handles));
}

/// CEP:WHAT: Loads the first MachineCode section as a J2Code.
/// CEP:WHY: The single-method reload path (docs/infrastructure/03 section 4).
/// CEP:STATUS: complete
/// CEP:FAILURE: named reject when the container has no MachineCode section.
/// CEP:ASSUMES: the reader passed the Rule-9 protocol.
/// CEP:COST: @cold. One scan + one payload parse.
/// CEP:EVIDENCE: code_cache_reload_parity in tests/test_snapshot.cpp.
Result<j2::J2Code> load_code_snapshot(const SnapshotReader& in,
                                      const std::vector<KlassHandle>& handles) {
    auto body = in.section_payload(SnapshotSection::MachineCode);
    if (!body) return std::unexpected(body.error());
    return take_code_payload(*body, handles);
}

/// CEP:WHAT: Loads + publishes through the normal W^X path (publish_j2).
/// CEP:WHY: ONE publication contract: reloaded code installs exactly like freshly compiled code (no second emission path).
/// CEP:STATUS: complete
/// CEP:FAILURE: rides through load + publish_j2's named failures.
/// CEP:ASSUMES: handles match the compile-time handle set.
/// CEP:COST: @cold. One W^X flip + relocation patching.
/// CEP:EVIDENCE: code_cache_reload_parity's executed reload in tests/test_snapshot.cpp.
Result<j2::J2Executable> publish_code_snapshot(
    const SnapshotReader& in, const std::vector<KlassHandle>& handles,
    CodeRange* range) {
    auto code = load_code_snapshot(in, handles);
    if (!code) return std::unexpected(code.error());
    return j2::publish_j2(*code, range);
}

// ---- profile persistence ---------------------------------------------------

namespace {

/// CEP:WHAT: True when a ProfileSlot never recorded anything.
/// CEP:WHY: The dump writes only meaningful lines (volumes are small, text formats).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure predicate).
/// CEP:ASSUMES: s is a slot record.
/// CEP:COST: @cold. Five compares.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip in tests/test_snapshot.cpp.
bool branch_slot_empty(const ugb::ProfileSlot& s) noexcept {
    return s.execution_count == 0 && s.failure_count == 0 &&
           s.type_ring_size == 0 && s.branch_counts[0] == 0 &&
           s.branch_counts[1] == 0;
}

/// CEP:WHAT: True when an IcSlot stayed uninitialized with no failures.
/// CEP:WHY: The dump writes only meaningful lines.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure predicate).
/// CEP:ASSUMES: s is a slot record.
/// CEP:COST: @cold. Two compares.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip in tests/test_snapshot.cpp.
bool ic_slot_empty(const ugb::IcSlot& s) noexcept {
    return s.state == ugb::IcState::Uninitialized && s.failure_count == 0;
}

/// CEP:WHAT: Splits profile text into non-empty lines.
/// CEP:WHY: The .ic/.branch formats are line-oriented text (docs/infrastructure/03 section 5).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure split).
/// CEP:ASSUMES: text is the profile file content.
/// CEP:COST: @cold. Linear in text.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip in tests/test_snapshot.cpp.
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const auto line = text.substr(
            pos, nl == std::string_view::npos ? text.size() - pos
                                              : nl - pos);
        if (!line.empty()) lines.push_back(line);
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return lines;
}

/// CEP:WHAT: Strict decimal u64 parse (overflow-checked).
/// CEP:WHY: Rule 9: profile fields are untrusted; garbage or oversized values reject the line.
/// CEP:STATUS: complete
/// CEP:FAILURE: returns false on any non-digit or overflow.
/// CEP:ASSUMES: s is one whitespace-free token.
/// CEP:COST: @cold. Linear in token length.
/// CEP:EVIDENCE: the malformed-profile rejects in tests/test_snapshot.cpp.
bool parse_u64(std::string_view s, uint64_t& out) noexcept {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (const char ch : s) {
        if (ch < '0' || ch > '9') return false;
        if (v > (UINT64_MAX - (ch - '0')) / 10) return false;
        v = v * 10 + static_cast<uint64_t>(ch - '0');
    }
    out = v;
    return true;
}

/// CEP:WHAT: Splits one profile line into whitespace-separated fields.
/// CEP:WHY: The profile formats are space-delimited (docs/infrastructure/03 section 5).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure split).
/// CEP:ASSUMES: line is one profile record.
/// CEP:COST: @cold. Linear in line length.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip in tests/test_snapshot.cpp.
std::vector<std::string_view> split_fields(std::string_view line) {
    std::vector<std::string_view> fields;
    size_t pos = 0;
    while (pos <= line.size()) {
        const size_t sp = line.find(' ', pos);
        const auto f = line.substr(
            pos, sp == std::string_view::npos ? line.size() - pos : sp - pos);
        if (!f.empty()) fields.push_back(f);
        if (sp == std::string_view::npos) break;
        pos = sp + 1;
    }
    return fields;
}

}  // namespace

namespace {

/// CEP:WHAT: The method's instruction count (the interpreter builds the same
///           table at first execution).
/// CEP:WHY: The JIT indexes profile slots by instruction index — a short
///           table would be an out-of-bounds read at compile time; a fresh
///           decoded module has not run yet, so ingest walks the stream.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (a malformed stream yields the partial count — the
///           same tolerance the interpreter's walk has).
/// CEP:ASSUMES: m.code is the method's encoded stream.
/// CEP:COST: @cold. One linear decode walk.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip's ingest into a fresh
///           module in tests/test_snapshot.cpp.
size_t method_instruction_count(const ugb::UGBMethod& m) {
    if (m.runtime_tables_ready) return m.profiles.size();
    ugb::InstructionStream stream(m.code.data(), m.code.size());
    size_t off = 0;
    size_t count = 0;
    while (off < m.code.size()) {
        ++count;
        ugb::Instruction instr;
        if (!stream.decode_at(off, instr)) break;
    }
    return count;
}

}  // namespace

/// CEP:WHAT: Dumps the per-method ProfileSlot tables (.branch text format).
/// CEP:WHY: PGO-AOT consumes production profiles; the dump is the frozen form (Rule 24; docs/infrastructure/03 section 5).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure dump).
/// CEP:ASSUMES: module is the warmed runtime module.
/// CEP:COST: @cold. Linear in non-empty slots.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip in tests/test_snapshot.cpp.
Result<std::string> dump_branch_profile(const ugb::UGBModule& module) {
    std::string out = "#vortex-branch-profile 1\n";
    for (size_t m = 0; m < module.method_table.size(); ++m) {
        const auto& method = module.method_table[m];
        for (size_t s = 0; s < method.profiles.size(); ++s) {
            const auto& slot = method.profiles[s];
            if (branch_slot_empty(slot)) continue;
            out += "b " + std::to_string(m) + " " + std::to_string(s) + " " +
                   std::to_string(slot.execution_count) + " " +
                   std::to_string(slot.failure_count) + " " +
                   std::to_string(slot.type_ring_size);
            for (uint8_t i = 0; i < slot.type_ring_size; ++i) {
                out += " " + std::to_string(slot.type_ring[i]);
            }
            out += " " + std::to_string(slot.branch_counts[0]) + " " +
                   std::to_string(slot.branch_counts[1]) + "\n";
        }
    }
    return out;
}

/// CEP:WHAT: Dumps the per-method IcSlot tables (.ic text format).
/// CEP:WHY: PGO-AOT consumes production profiles; the dump is the frozen form.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure dump).
/// CEP:ASSUMES: module is the warmed runtime module.
/// CEP:COST: @cold. Linear in non-empty slots.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip in tests/test_snapshot.cpp.
Result<std::string> dump_ic_profile(const ugb::UGBModule& module) {
    std::string out = "#vortex-ic-profile 1\n";
    for (size_t m = 0; m < module.method_table.size(); ++m) {
        const auto& method = module.method_table[m];
        for (size_t s = 0; s < method.ics.size(); ++s) {
            const auto& slot = method.ics[s];
            if (ic_slot_empty(slot)) continue;
            out += "i " + std::to_string(m) + " " + std::to_string(s) + " " +
                   std::to_string(static_cast<uint32_t>(slot.state)) + " " +
                   std::to_string(slot.poly_count) + " " +
                   std::to_string(slot.failure_count) + " " +
                   std::to_string(slot.mono.klass_or_shape_id) + ":" +
                   std::to_string(slot.mono.target);
            for (uint8_t i = 0; i < slot.poly_count; ++i) {
                out += " " + std::to_string(slot.poly[i].klass_or_shape_id) +
                       ":" + std::to_string(slot.poly[i].target);
            }
            out += "\n";
        }
    }
    return out;
}

/// CEP:WHAT: Ingests a .branch dump into the module's slot tables, all-or-nothing.
/// CEP:WHY: Rule 9: every line validates (shape, ranges, ring size) BEFORE the module mutates; sizing covers the full instruction count so the JIT's slot indexing stays in bounds.
/// CEP:STATUS: complete
/// CEP:FAILURE: named rejects on malformed lines, bad ids, and out-of-range slots; the module is untouched on rejection.
/// CEP:ASSUMES: text carries the version header.
/// CEP:COST: @cold. Linear in the file + one instruction-count walk per referenced method.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip's round trip + negatives in tests/test_snapshot.cpp.
Result<void> ingest_branch_profile(std::string_view text,
                                   ugb::UGBModule& module) {
    if (text.rfind("#vortex-branch-profile 1", 0) != 0) {
        return std::unexpected(snap_err("branch profile: bad header"));
    }
    struct Entry {
        uint32_t mid, slot, fail;
        uint64_t exec;
        uint8_t ring_size;
        uint8_t ring[ugb::kProfileTypeRingDepth];
        uint32_t b0, b1;
    };
    std::vector<Entry> entries;
    for (const auto line : split_lines(text)) {
        if (line[0] == '#') continue;
        const auto f = split_fields(line);
        uint64_t mid = 0, slot = 0, exec = 0, fail = 0, ring = 0, b0 = 0,
                 b1 = 0;
        if (f.size() < 8 || f[0] != "b" || !parse_u64(f[1], mid) ||
            !parse_u64(f[2], slot) || !parse_u64(f[3], exec) ||
            !parse_u64(f[4], fail) || !parse_u64(f[5], ring) ||
            !parse_u64(f[f.size() - 2], b0) ||
            !parse_u64(f[f.size() - 1], b1)) {
            return std::unexpected(
                snap_err("branch profile: malformed line"));
        }
        // u64 fields narrowed to u32/u8 reject instead of truncating
        // (review finding 15-e-b F8, probe-proven: 2^32 aliased method 0).
        if (mid > 0xFFFFFFFFull || slot > 0xFFFFFFFFull ||
            fail > 0xFFFFFFFFull || b0 > 0xFFFFFFFFull ||
            b1 > 0xFFFFFFFFull) {
            return std::unexpected(snap_err("branch profile: field too large"));
        }
        const size_t tag_count = f.size() - 8;
        if (ring > ugb::kProfileTypeRingDepth || tag_count != ring) {
            return std::unexpected(
                snap_err("branch profile: type ring size invalid"));
        }
        Entry e{};
        e.mid = static_cast<uint32_t>(mid);
        e.slot = static_cast<uint32_t>(slot);
        e.exec = exec;
        e.fail = static_cast<uint32_t>(fail);
        e.ring_size = static_cast<uint8_t>(ring);
        for (size_t i = 0; i < tag_count; ++i) {
            uint64_t tag = 0;
            if (!parse_u64(f[6 + i], tag) || tag > 0xFF) {
                return std::unexpected(
                    snap_err("branch profile: tag invalid"));
            }
            e.ring[i] = static_cast<uint8_t>(tag);
        }
        e.b0 = static_cast<uint32_t>(b0);
        e.b1 = static_cast<uint32_t>(b1);
        entries.push_back(e);
    }
    // Validate EVERYTHING against the would-be table geometry BEFORE the
    // module mutates — the all-or-nothing contract (review finding
    // 15-e-b F6, probe-proven: a late reject left earlier entries applied).
    for (const auto& e : entries) {
        if (e.mid >= module.method_table.size()) {
            return std::unexpected(
                snap_err("branch profile: method id out of range"));
        }
        if (e.slot >= method_instruction_count(
                          module.method_table[e.mid])) {
            return std::unexpected(
                snap_err("branch profile: slot index out of range"));
        }
        for (const auto& prior : entries) {
            if (&prior == &e) break;
            if (prior.mid == e.mid && prior.slot == e.slot) {
                return std::unexpected(
                    snap_err("branch profile: duplicate slot line"));
            }
        }
    }
    for (const auto& e : entries) {
        auto& method = module.method_table[e.mid];
        method.ensure_runtime_tables(method_instruction_count(method));
        auto& s = method.profiles[e.slot];
        s.execution_count = e.exec;
        s.failure_count = e.fail;
        s.type_ring_size = e.ring_size;
        for (uint8_t i = 0; i < e.ring_size; ++i) s.type_ring[i] = e.ring[i];
        s.branch_counts[0] = e.b0;
        s.branch_counts[1] = e.b1;
    }
    return support::ok();
}

/// CEP:WHAT: Ingests an .ic dump into the module's IC tables, all-or-nothing.
/// CEP:WHY: Same all-or-nothing contract as the branch ingest (docs/infrastructure/03 section 5).
/// CEP:STATUS: complete
/// CEP:FAILURE: named rejects on malformed lines, bad ids, state/range violations; the module is untouched on rejection.
/// CEP:ASSUMES: text carries the version header.
/// CEP:COST: @cold. Linear in the file + one instruction-count walk per referenced method.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip's round trip + negatives in tests/test_snapshot.cpp.
Result<void> ingest_ic_profile(std::string_view text, ugb::UGBModule& module) {
    if (text.rfind("#vortex-ic-profile 1", 0) != 0) {
        return std::unexpected(snap_err("ic profile: bad header"));
    }
    struct Entry {
        uint32_t mid, slot, fail;
        ugb::IcState state;
        uint8_t poly;
        ugb::IcEntry mono;
        std::vector<ugb::IcEntry> polys;
    };
    std::vector<Entry> entries;
    for (const auto line : split_lines(text)) {
        if (line[0] == '#') continue;
        const auto f = split_fields(line);
        if (f.size() < 7 || f[0] != "i") {
            return std::unexpected(snap_err("ic profile: malformed line"));
        }
        uint64_t mid = 0, slot = 0, state = 0, poly = 0, fail = 0;
        if (!parse_u64(f[1], mid) || !parse_u64(f[2], slot) ||
            !parse_u64(f[3], state) || !parse_u64(f[4], poly) ||
            !parse_u64(f[5], fail)) {
            return std::unexpected(snap_err("ic profile: field invalid"));
        }
        if (mid > 0xFFFFFFFFull || slot > 0xFFFFFFFFull ||
            fail > 0xFFFFFFFFull) {
            return std::unexpected(snap_err("ic profile: field too large"));
        }
        if (state > 3 || poly > ugb::kIcPolyCapacity ||
            f.size() != 7 + poly) {
            return std::unexpected(snap_err("ic profile: shape invalid"));
        }
        Entry e{};
        e.mid = static_cast<uint32_t>(mid);
        e.slot = static_cast<uint32_t>(slot);
        e.state = static_cast<ugb::IcState>(state);
        e.poly = static_cast<uint8_t>(poly);
        e.fail = static_cast<uint32_t>(fail);
        const auto parse_entry = [&](std::string_view tok,
                                     ugb::IcEntry& out) -> bool {
            const size_t colon = tok.find(':');
            if (colon == std::string_view::npos) return false;
            uint64_t k = 0, t = 0;
            if (!parse_u64(tok.substr(0, colon), k) ||
                !parse_u64(tok.substr(colon + 1), t)) {
                return false;
            }
            // u64 -> u32 narrows reject instead of truncating (review
            // 15-g-b N3, probe-proven: 2^32+1 aliased to 1).
            if (k > 0xFFFFFFFFull || t > 0xFFFFFFFFull) return false;
            out.klass_or_shape_id = static_cast<uint32_t>(k);
            out.target = static_cast<uint32_t>(t);
            return true;
        };
        if (!parse_entry(f[6], e.mono)) {
            return std::unexpected(snap_err("ic profile: mono entry invalid"));
        }
        for (size_t i = 0; i < poly; ++i) {
            ugb::IcEntry p;
            if (!parse_entry(f[7 + i], p)) {
                return std::unexpected(snap_err("ic profile: poly invalid"));
            }
            e.polys.push_back(p);
        }
        entries.push_back(std::move(e));
    }
    for (const auto& e : entries) {
        if (e.mid >= module.method_table.size()) {
            return std::unexpected(
                snap_err("ic profile: method id out of range"));
        }
        if (e.slot >= method_instruction_count(
                          module.method_table[e.mid])) {
            return std::unexpected(
                snap_err("ic profile: slot index out of range"));
        }
        for (const auto& prior : entries) {
            if (&prior == &e) break;
            if (prior.mid == e.mid && prior.slot == e.slot) {
                return std::unexpected(
                    snap_err("ic profile: duplicate slot line"));
            }
        }
    }
    for (const auto& e : entries) {
        auto& method = module.method_table[e.mid];
        method.ensure_runtime_tables(method_instruction_count(method));
        auto& s = method.ics[e.slot];
        s.state = e.state;
        s.poly_count = e.poly;
        s.failure_count = e.fail;
        s.mono = e.mono;
        for (uint8_t i = 0; i < e.poly; ++i) s.poly[i] = e.polys[i];
    }
    return support::ok();
}

// ---- the PGO-AOT pipeline ---------------------------------------------------

namespace {

/// CEP:WHAT: Reads a whole file (module bytes or profile text inputs).
/// CEP:WHY: The AOT pipeline's inputs are files; open failures are named (docs/infrastructure/03 section 6).
/// CEP:STATUS: complete
/// CEP:FAILURE: named reject when the file cannot be opened.
/// CEP:ASSUMES: the path is readable by the calling process.
/// CEP:COST: @cold. One sequential read.
/// CEP:EVIDENCE: aot_pipeline_deterministic in tests/test_snapshot.cpp.
Result<std::vector<uint8_t>> read_file_bytes(const std::string& path,
                                             const char* what) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::unexpected(aot_err(std::string("cannot open ") + what +
                                       ": " + path));
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    return bytes;
}

}  // namespace

/// CEP:WHAT: The offline PGO-AOT pipeline: module + profiles in, one verified container out.
/// CEP:WHY: Method selection is profile-licensed (Rule 24's frozen form); compilation rides the J4 deterministic engine; the manifest fingerprints every input (docs/infrastructure/03 section 6).
/// CEP:STATUS: complete
/// CEP:FAILURE: named rejects on unreadable/undecodable inputs, ingest failures, an empty selection, and compile refusals.
/// CEP:ASSUMES: the klass handles describe the deployment target's runtime.
/// CEP:COST: @cold. J4 compiles for the selected methods + one container write.
/// CEP:EVIDENCE: aot_pipeline_deterministic (byte-identical artifacts) in tests/test_snapshot.cpp.
Result<std::string> AotPipeline::emit_shared_library(
    const PgoProfileInput& input, const std::string& output_path) {
    // 1. Module + profiles in (Rule 9: validated ingest, all-or-nothing).
    auto module_bytes = read_file_bytes(input.module_path, "module");
    if (!module_bytes) return std::unexpected(module_bytes.error());
    auto ic_bytes = read_file_bytes(input.ic_profile_path, "ic profile");
    if (!ic_bytes) return std::unexpected(ic_bytes.error());
    auto branch_bytes =
        read_file_bytes(input.branch_profile_path, "branch profile");
    if (!branch_bytes) return std::unexpected(branch_bytes.error());

    ugb::UGBModule module;
    ugb::DecodeErrorInfo decode_err{};
    if (!ugb::decode_module(module_bytes->data(), module_bytes->size(),
                            module, decode_err)) {
        return std::unexpected(
            aot_err("module decode failed: " + decode_err.message));
    }
    // compile_j4's contract: the module is VERIFIED and its capabilities
    // are supported before any compile runs (Rule 3; review finding
    // 15-e-b F9). The offline pipeline negotiates against the engine's
    // advertised set, exactly like the interpreter's run boundary.
    if (auto v = ugb::verify_module(module); !v) {
        return std::unexpected(aot_err("module verify failed: " +
                                       v.error().message));
    }
    {
        const ugb::CapabilitySet advertised =
            ugb::engine_advertised_capabilities();
        for (const auto& m : module.method_table) {
            for (uint8_t raw : m.required_capabilities) {
                if (!ugb::is_valid_capability(raw) ||
                    !advertised.supports_raw(raw)) {
                    return std::unexpected(aot_err(
                        "method '" + m.name +
                        "' requires an unsupported capability"));
                }
            }
        }
    }
    {
        std::string ic_text(reinterpret_cast<const char*>(ic_bytes->data()),
                            ic_bytes->size());
        std::string branch_text(
            reinterpret_cast<const char*>(branch_bytes->data()),
            branch_bytes->size());
        auto ing = ingest_ic_profile(ic_text, module);
        if (!ing) return std::unexpected(ing.error());
        ing = ingest_branch_profile(branch_text, module);
        if (!ing) return std::unexpected(ing.error());
    }

    // 2. Method selection: the profile IS the license (Rule 24's frozen
    // profile snapshot — the dump is its frozen form).
    const std::vector<void*> klass_addrs = [&] {
        std::vector<void*> v;
        v.reserve(input.klass_handles.size());
        for (const auto& h : input.klass_handles) {
            v.push_back(const_cast<void*>(h.address));
        }
        return v;
    }();
    const void* double_klass = nullptr;
    for (const auto& h : input.klass_handles) {
        if (h.name == "double") double_klass = h.address;
    }

    ugb::UGBModule* mod = &module;
    std::vector<uint32_t> selected;
    for (size_t m = 0; m < mod->method_table.size(); ++m) {
        uint64_t activity = 0;
        for (const auto& slot : mod->method_table[m].profiles) {
            activity += slot.execution_count;
        }
        if (activity >= input.min_execution_count) {
            selected.push_back(static_cast<uint32_t>(m));
        }
    }
    if (selected.empty()) {
        return std::unexpected(
            aot_err("no method meets the execution-count threshold"));
    }

    // 3-4. J4 deterministic compile per selected method, one container out.
    SnapshotWriter writer;
    Manifest manifest = snapshot_default_manifest();
    manifest.dependencies.push_back(
        ManifestDependency{"module", fnv_bytes(*module_bytes)});
    manifest.dependencies.push_back(
        ManifestDependency{"ic-profile", fnv_bytes(*ic_bytes)});
    manifest.dependencies.push_back(
        ManifestDependency{"branch-profile", fnv_bytes(*branch_bytes)});
    auto mst = writer.set_manifest(manifest);
    if (!mst) return std::unexpected(mst.error());

    for (uint32_t mid : selected) {
        auto& method = mod->method_table[mid];
        j2::J2Job job;
        job.module = mod;
        job.method_id = mid;
        job.profiles = &method.profiles;
        job.ics = &method.ics;
        job.klass_addrs = &klass_addrs;
        job.double_klass = double_klass;
        auto code = j4::compile_j4(job);
        if (!code) {
            return std::unexpected(aot_err("compile failed for method " +
                                           std::to_string(mid) + ": " +
                                           code.error().message));
        }
        auto ws = write_code_snapshot(*code, input.klass_handles, writer);
        if (!ws) return std::unexpected(ws.error());
    }
    auto wf = writer.write_file(output_path);
    if (!wf) return std::unexpected(wf.error());
    return output_path;
}

/// CEP:WHAT: The deployment boot: verify container, verify manifest, load + publish every method section.
/// CEP:WHY: Rule 98's ordered checks run before any code becomes executable; publication shares the JIT's W^X path.
/// CEP:STATUS: complete
/// CEP:FAILURE: named rejects at every verification layer and on publish failures.
/// CEP:ASSUMES: expected carries the loader's identity and dependency fingerprints.
/// CEP:COST: @cold. Container verification + one publish per method.
/// CEP:EVIDENCE: aot_artifact_load_execute_parity (parity + drift reject) in tests/test_snapshot.cpp.
Result<std::unique_ptr<AotArtifact>> AotPipeline::load_artifact(
    const std::string& path, const Manifest& expected,
    const std::vector<KlassHandle>& current_handles, CodeRange* range) {
    auto reader = SnapshotReader::open(path);
    if (!reader) return std::unexpected(reader.error());
    auto actual = reader->manifest();
    if (!actual) return std::unexpected(actual.error());
    auto verdict = ManifestVerifier::verify(*actual, expected);
    if (!verdict) return std::unexpected(verdict.error());

    auto artifact = std::make_unique<AotArtifact>();
    for (const auto& name : reader->section_names(SnapshotSection::MachineCode)) {
        auto body = reader->section_payload(SnapshotSection::MachineCode,
                                            name);
        if (!body) return std::unexpected(body.error());
        auto code = take_code_payload(*body, current_handles);
        if (!code) return std::unexpected(code.error());
        artifact->method_ids.push_back(code->method_id);
        auto ex = j2::publish_j2(*code, range);
        if (!ex) return std::unexpected(ex.error());
        artifact->executables.push_back(
            std::make_unique<j2::J2Executable>(std::move(*ex)));
    }
    if (artifact->executables.empty()) {
        return std::unexpected(aot_err("artifact carries no machine code"));
    }
    return artifact;
}

}  // namespace vortex::infra



