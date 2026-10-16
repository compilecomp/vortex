// CEP:FILE: include/vortex/infra/snapshot.hpp
// CEP:WHAT: Startup snapshots & the AOT pipeline (docs/infrastructure/
//           03-snapshots-aot.md): the versioned container, the Rule-97
//           compatibility manifest, the Rule-98 verification protocol, the
//           heap snapshot writer/reader, the shared code cache round trip,
//           profile persistence (.ic/.branch), and the offline PGO-AOT
//           pipeline over the J4 deterministic engine.
// CEP:WHY: Parsing and compiling core libraries on every startup is waste;
//           snapshots and AOT artifacts remove it WITHOUT creating a second
//           compilation contract — every code path reloads through the same
//           publish machinery the JIT uses (one semantic contract, Rule 18).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Every parse/verify/restore failure is a named Diagnostic
//           ("snapshot: <what>" / "aot: <what>") — malformed or incompatible
//           artifacts are REJECTED, never partially loaded, never a crash
//           (Rule 9). Writers refuse reuse-after-finalize with named errors.
// CEP:ASSUMES: Artifacts are untrusted bytes; the heap snapshot captures a
//           quiescent state (Rule 24: no guest execution during capture);
//           restore is layout-deterministic (same bytes -> same heap-relative
//           offsets and klass layouts — raw base pointers are process data).
// CEP:COST: @cold. Container finalize/load are linear in artifact bytes with
//           two FNV-1a hash passes. Heap snapshot/restore are O(objects).
//           No hot-path cost: nothing here runs between guest instructions.
// CEP:EVIDENCE: tests/test_snapshot.cpp — container rejects (Rule 9),
//           manifest field mismatches (Rule 98), heap round-trip parity +
//           restore determinism, code-cache reload parity + handle
//           verification, profile round trips, AOT determinism + load parity.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/infra/code_range.hpp"
#include "vortex/support/result.hpp"

namespace vortex {

class HeapObject;
class Klass;
class KlassRegistry;

namespace gc {
class Heap;
}
namespace j2 {
struct J2Code;
struct J2Executable;
}
namespace ugb {
class UGBModule;
}

namespace infra {

// ---- constants -----------------------------------------------------------
// Rule 97 manifest identity. The compiler version bumps whenever the pass
// pipeline's observable contract changes; the pipeline hash covers the J4
// fixed driver (60 named stages, docs/tier-j3.md section 2).
inline constexpr uint32_t kSnapshotUgbVersion = 1;
inline constexpr uint32_t kSnapshotCompilerVersion = 1;
inline constexpr uint32_t kSnapshotSecurityPolicyVersion = 1;
inline constexpr size_t kSnapshotAotMinExecutionCount = 1000;

/// Snapshot container sections (docs/infrastructure/03: heap snapshots).
enum class SnapshotSection : uint8_t {
    Objects = 0,
    Shapes,
    InlineCaches,
    MachineCode,
    Metadata,
};

/// The container magic ('V3SN'), named once (CEP&CC 11.3).
inline constexpr uint32_t kSnapshotMagic = 0x5633534Eu;

struct SnapshotHeader {
    uint32_t magic = kSnapshotMagic;
    uint16_t version_major = 1;
    uint16_t version_minor = 1;
    uint32_t section_count = 0;
};
static_assert(sizeof(SnapshotHeader) == 12, "container header is 12 bytes");

/// FNV-1a 64 — the container's only hash (section payloads + whole file).
uint64_t fnv1a64(const uint8_t* data, size_t size) noexcept;

// ---- the compatibility manifest (Rules 97/98) ----------------------------

struct ManifestDependency {
    std::string name;
    uint64_t fingerprint = 0;
};

/// The nine Rule-97 fields, in serialization order. `ManifestVerifier`
/// compares them in THIS order and names the first mismatch.
struct Manifest {
    uint32_t ugb_version = kSnapshotUgbVersion;
    uint32_t compiler_version = kSnapshotCompilerVersion;
    uint64_t pass_pipeline_hash = 0;
    std::string target_arch = "x86-64";
    std::string target_feature_set = "base";
    uint64_t abi_hash = 0;
    uint64_t runtime_config_hash = 0;
    std::vector<ManifestDependency> dependencies;
    uint32_t security_policy_version = kSnapshotSecurityPolicyVersion;
};

/// The manifest identity of THIS build (pipeline/abi/config hashes computed
/// from the build's stable constants — docs/infrastructure/03 section 2).
Manifest snapshot_default_manifest();

/// Serializes the manifest (length-prefixed fields, spec order).
std::vector<uint8_t> build_manifest_payload(const Manifest& manifest);
/// Parses a manifest payload. Named failure on any structure violation.
support::Result<Manifest> parse_manifest_payload(
    std::span<const uint8_t> payload);

/// CEP:WHAT: Rule-98 field-by-field comparison; feature sets check subset
/// semantics (every artifact feature must be a loader feature).
/// CEP:WHY: An artifact that loads on a mismatched runtime is a silent ABI
/// break; the verifier makes every mismatch a NAMED rejection.
/// CEP:STATUS: complete
/// CEP:FAILURE: Named diagnostic naming the field and both values.
/// CEP:ASSUMES: expected carries the LOADER's identity.
/// CEP:COST: @cold. Field compares + one subset pass over short strings.
/// CEP:EVIDENCE: snapshot_manifest_mismatch_rejected (one negative per field).
class ManifestVerifier {
public:
    static support::Result<void> verify(const Manifest& actual,
                                        const Manifest& expected);
};

/// True when every space-separated feature in `artifact` appears in
/// `loader` ("base" always matches itself; an empty artifact set = "base").
bool feature_set_subset(std::string_view artifact, std::string_view loader);

// ---- the container -------------------------------------------------------

/// Heap + code serialization with relocatable pointers and verification
/// hashes (docs/infrastructure/03 section 1). Sections are opaque bodies;
/// finalize() prepends each body's FNV-1a hash and the file trailer.
class SnapshotWriter {
public:
    support::Result<void> add_section(SnapshotSection kind,
                                      std::vector<uint8_t> payload);
    /// Named form (required for multiple sections of one kind — the AOT
    /// artifact names each method section "method:<id>").
    support::Result<void> add_section_named(SnapshotSection kind,
                                            std::string_view name,
                                            std::vector<uint8_t> payload);
    /// Registers the manifest: finalize() emits it as the FIRST Metadata
    /// section (the loader's manifest() reads that one).
    support::Result<void> set_manifest(const Manifest& manifest);
    support::Result<std::vector<uint8_t>> finalize();
    /// finalize() + write to `path` (truncate). Named I/O failures.
    support::Result<void> write_file(const std::string& path);
    size_t total_bytes() const noexcept { return total_bytes_; }

private:
    support::Result<void> add_section_impl(SnapshotSection kind,
                                           std::string_view name,
                                           std::vector<uint8_t> payload);
    SnapshotHeader header_{};
    struct Entry {
        SnapshotSection kind;
        std::string name;
        std::vector<uint8_t> body;
    };
    std::vector<Entry> sections_;
    std::vector<uint8_t> manifest_payload_;
    bool has_manifest_ = false;
    bool finalized_ = false;
    size_t total_bytes_ = 0;
};

/// Memory-maps (reads) a snapshot at startup and runs the full Rule-9
/// integrity protocol before any accessor succeeds.
class SnapshotReader {
public:
    /// Reads `path` and verifies: magic, version, file hash, directory
    /// bounds + non-overlap, per-section hashes. Any violation is a named
    /// reject and the reader is never constructed.
    static support::Result<SnapshotReader> open(const std::string& path);

    support::Result<size_t> section_count() const;
    /// The names of the sections of `kind`, in file order (AOT artifacts
    /// name their MachineCode sections "method:<id>").
    std::vector<std::string> section_names(SnapshotSection kind) const;
    /// The first section of `kind` whose name matches (empty `name` matches
    /// the first anonymous/any section of that kind).
    support::Result<std::span<const uint8_t>> section_payload(
        SnapshotSection kind, std::string_view name = {}) const;
    /// The manifest = the FIRST Metadata section's payload, parsed.
    support::Result<Manifest> manifest() const;
    /// Raw container bytes (the heap/code loaders slice into them).
    const std::vector<uint8_t>& bytes() const noexcept { return *bytes_; }

private:
    struct Entry {
        SnapshotSection kind;
        std::string name;
        uint64_t offset;  // file offset of the payload body (past the hash)
        uint64_t size;
    };
    std::shared_ptr<std::vector<uint8_t>> bytes_;
    std::vector<Entry> entries_;
};

// ---- heap snapshots (docs/infrastructure/03 section 3) --------------------

/// Result payloads for a heap restore: fresh heap + registry, the object
/// table (snapshot order), and the double klass.
struct RestoredHeap {
    std::unique_ptr<gc::Heap> heap;
    std::unique_ptr<KlassRegistry> klasses;
    std::vector<HeapObject*> objects;  // snapshot index -> restored object
    Klass* double_klass = nullptr;
};

struct HeapRestoreConfig {
    size_t young_bytes = 1 * 1024 * 1024;  // kDefaultYoungBytes shape
    size_t old_bytes = 8 * 1024 * 1024;    // kDefaultOldBytes shape
};

/// Serializes `klasses` (+ `heap.double_klass()` when distinct) into the
/// Shapes section and the object graph into the Objects section. Pointer
/// words are rewritten to the swizzled form
/// (0xFFFF'FFFF'0000'0000 | (index << 4) | 0x5) — the 0x5 tag nibble is a
/// value no legal TaggedValue can take, so negative Smis (which share the
/// high word) still decode as Smis.
support::Result<void> write_heap_snapshot(gc::Heap& heap,
                                          KlassRegistry& klasses,
                                          SnapshotWriter& out);
/// Rebuilds the heap: klass table first, then objects in snapshot order
/// through the normal allocation paths, then the pointer-word rewrite.
support::Result<RestoredHeap> restore_heap_snapshot(
    const SnapshotReader& in, const HeapRestoreConfig& cfg = {});

/// The klass-handle table the code cache verifies against: one entry per
/// registry klass plus the heap's double klass (name -> live address).
struct KlassHandle {
    std::string name;
    const void* address = nullptr;
};
std::vector<KlassHandle> heap_klass_handles(KlassRegistry& klasses,
                                            gc::Heap& heap);

// ---- the shared code cache (docs/infrastructure/03 section 4) -------------

/// Serializes one compiled method: code bytes (address-free, Rule 56),
/// entry/OSR offsets, GC maps, the LIVE deopt records (frames + remats),
/// the region table, the record relocations, and the klass-handle table
/// the compile baked. The handle table is verified at load (Rule 98).
support::Result<void> write_code_snapshot(const j2::J2Code& code,
                                          const std::vector<KlassHandle>& handles,
                                          SnapshotWriter& out);
/// Rebuilds the J2Code. Every baked handle must match `current_handles` by
/// NAME and ADDRESS (the restore-determinism contract makes them equal);
/// any drift is a named reject.
support::Result<j2::J2Code> load_code_snapshot(
    const SnapshotReader& in, const std::vector<KlassHandle>& current_handles);
/// load + publish through the normal W^X path (one publication contract).
support::Result<j2::J2Executable> publish_code_snapshot(
    const SnapshotReader& in, const std::vector<KlassHandle>& current_handles,
    CodeRange* range = nullptr);

// ---- profile persistence (docs/infrastructure/03 section 5) ---------------

/// Dumps the per-method ProfileSlot tables (`.branch` text format).
support::Result<std::string> dump_branch_profile(const ugb::UGBModule& module);
/// Dumps the per-method IcSlot tables (`.ic` text format).
support::Result<std::string> dump_ic_profile(const ugb::UGBModule& module);
/// Ingests a `.branch` dump into `module`'s slot tables. All-or-nothing:
/// any malformed line rejects the whole file (Rule 9) and leaves the
/// module untouched.
support::Result<void> ingest_branch_profile(std::string_view text,
                                            ugb::UGBModule& module);
/// Ingests an `.ic` dump (same all-or-nothing contract).
support::Result<void> ingest_ic_profile(std::string_view text,
                                        ugb::UGBModule& module);

// ---- the PGO-AOT pipeline (docs/infrastructure/03 section 6) --------------

/// PGO-AOT: ingests production profiles (.ic/.branch) and runs the J4
/// deterministic pipeline offline to emit a native shared library.
/// `module_path` names the UGB module; `klass_handles` carries the compile
/// target's live klass addresses (the loader verifies them — the artifact
/// and its heap snapshot ship together).
struct PgoProfileInput {
    std::string module_path;
    std::string ic_profile_path;
    std::string branch_profile_path;
    std::vector<KlassHandle> klass_handles;
    size_t min_execution_count = kSnapshotAotMinExecutionCount;
};

struct AotArtifact {
    /// One executable per compiled method, section order (method id order).
    std::vector<std::unique_ptr<j2::J2Executable>> executables;
    std::vector<uint32_t> method_ids;  // parallel to executables
};

class AotPipeline {
public:
    /// Module + profiles in, artifact container out (MachineCode sections
    /// named "method:<id>" + the Rule-97 manifest, whose dependency
    /// fingerprints cover the module and both profile files). Two runs over
    /// identical inputs produce byte-identical artifacts (J4 determinism).
    static support::Result<std::string> emit_shared_library(
        const PgoProfileInput& input, const std::string& output_path);
    /// The deployment boot: open + verify (Rule 9), manifest verify against
    /// `expected` (Rule 98), per-method handle verify + W^X publish.
    static support::Result<std::unique_ptr<AotArtifact>> load_artifact(
        const std::string& path, const Manifest& expected,
        const std::vector<KlassHandle>& current_handles,
        CodeRange* range = nullptr);
};

}  // namespace vortex::infra

}  // namespace vortex
