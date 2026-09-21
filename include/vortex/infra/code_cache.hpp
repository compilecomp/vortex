// Infra 8 — Executable Memory & I-Cache Manager
// (docs/infrastructure/08-icache-codecache.md).
//
// M0 implements segmented allocation over the W^X manager with tier/kind
// separation and metadata attachment. Sweeping/compaction, NUMA placement and
// huge pages are contract-stubbed.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "vortex/infra/security.hpp"
#include "vortex/support/tier.hpp"

namespace vortex::infra {

enum class CodeSegmentKind : uint8_t {
    Baseline,
    Optimized,
    Outlined,
    Stubs,
    BarrierStubs,
    DeoptTrampolines,
    ContinuationStubs,
};

/// A published code allocation. Metadata travels with the code
/// (GC maps, deopt records, region tables).
struct CodeAllocation {
    uint32_t id = 0;
    CodeSegmentKind kind = CodeSegmentKind::Baseline;
    vortex::Tier tier = vortex::Tier::J1;
    uint32_t method_id = 0;
    size_t offset = 0;
    size_t size = 0;
    std::vector<uint8_t> gc_maps;
    std::vector<uint8_t> deopt_records;
};

/// The code cache: segmented store over WritableCodeMemory
/// (docs/ir-son-ciog.md runtime section: segmentation, eviction, tier
/// demotion, hot/cold clustering). Publication is W^X-safe by construction.
class CodeCache {
public:
    explicit CodeCache(size_t total_bytes);

    support::Result<CodeAllocation> allocate(
        CodeSegmentKind kind, vortex::Tier tier, uint32_t method_id,
        std::span<const uint8_t> code);

    /// Returns the executable address of a published allocation.
    const uint8_t* code_of(const CodeAllocation& alloc) const noexcept;

    size_t used_bytes() const noexcept { return used_; }
    size_t total_bytes() const noexcept { return memory_.size(); }
    size_t allocation_count() const noexcept { return allocations_.size(); }

private:
    WritableCodeMemory memory_;
    size_t used_ = 0;
    std::vector<CodeAllocation> allocations_;
};

/// I-cache synchronization policy (ARM64: DC CVAU + IC IVAU batching;
/// x86-64: serialization after code publication). Configuration type.
struct IcSyncPolicy {
    bool batch_cache_maintenance = true;
    size_t batch_threshold = 64;
};

/// NUMA code placement + huge page policy (contract stubs).
struct NumaCodePlacer {
    uint32_t preferred_node = 0;
    bool local_placement = false;
};

struct HugePagePolicy {
    bool transparent_huge_pages = false;
    size_t page_bytes = 2 * 1024 * 1024;
};

/// Background cold-region sweeper/compactor. Contract stub (docs/roadmap.md M8).
class CodeSweeper {
public:
    explicit CodeSweeper(CodeCache& cache) noexcept : cache_(cache) {}
    support::Result<size_t> sweep_cold_regions();

private:
    CodeCache& cache_;
};

}  // namespace vortex::infra
