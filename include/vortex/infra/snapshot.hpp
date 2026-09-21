// Infra 3 — Startup Snapshots & AOT Pipeline
// (docs/infrastructure/03-snapshots-aot.md). Contract-stubbed in M0; the
// container format is fixed now so snapshot support can land without breaking
// the runtime.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vortex/support/result.hpp"

namespace vortex::infra {

/// Snapshot container sections (docs/infrastructure/03: heap snapshots).
enum class SnapshotSection : uint8_t {
    Objects,
    Shapes,
    InlineCaches,
    MachineCode,
    Metadata,
};

struct SnapshotHeader {
    uint32_t magic = 0x5633534E;  // 'V3SN'
    uint16_t version_major = 0;
    uint16_t version_minor = 1;
    uint32_t section_count = 0;
};

/// Heap + code serialization with relocatable pointers and verification
/// hashes. Contract stub (docs/roadmap.md, M6).
class SnapshotWriter {
public:
    support::Result<void> add_section(SnapshotSection kind,
                                      std::vector<uint8_t> payload);
    support::Result<std::vector<uint8_t>> finalize();
    size_t total_bytes() const noexcept { return total_bytes_; }

private:
    SnapshotHeader header_{};
    std::vector<std::pair<SnapshotSection, std::vector<uint8_t>>> sections_;
    size_t total_bytes_ = 0;
};

/// Memory-maps a snapshot at startup, skipping T0/J1 for core code.
/// Contract stub.
class SnapshotReader {
public:
    static support::Result<SnapshotReader> open(const std::string& path);
    support::Result<size_t> section_count() const;
};

/// Shared code cache configuration (read-only mapped segments, CrossGen-style).
struct SharedCodeCacheConfig {
    std::string cache_path;
    size_t max_bytes = 64 << 20;
    bool verify_hash_on_map = true;
};

/// PGO-AOT: ingests production profiles (.ic/.branch) and runs the J4
/// deterministic pipeline offline to emit a native shared library. Contract stub.
struct PgoProfileInput {
    std::string ic_profile_path;
    std::string branch_profile_path;
};

class AotPipeline {
public:
    static support::Result<std::string> emit_shared_library(
        const PgoProfileInput& input, const std::string& output_path);
};

}  // namespace vortex::infra
