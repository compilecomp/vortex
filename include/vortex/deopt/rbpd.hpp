// RBPD — Region-Based Partial Deoptimization (docs/deopt-rbpd.md).
// M0 implements the metadata substrate: region descriptors, escape sets,
// partial deopt records, failure counters, and the region table the
// dependency/invalidation engine patches against. The deopt trampolines and
// region recompiler are contract-stubbed (docs/roadmap.md, M3).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vortex::deopt {

/// Region kinds (docs/deopt-rbpd.md section 1).
enum class RegionKind : uint8_t {
    Hot,
    Cold,
    Deopt,
    Barrier,
    SlowPath,
    InlineFallback,
    UncommonTrap,
    Exception,
};

/// Escape location (docs/deopt-rbpd.md section 3).
struct EscapeLocation {
    enum class Loc : uint8_t { Reg, Stack, Const };
    uint16_t virtual_register = 0;
    Loc location = Loc::Reg;
    uint32_t payload = 0;  // physical reg | stack offset | constant index
};

/// Partial deopt record (docs/deopt-rbpd.md section 3).
struct PartialDeoptRecord {
    uint32_t region_id = 0;
    uint32_t bytecode_resume_pc = 0;
    std::vector<EscapeLocation> escape_locations;
    uint32_t successor_region_id = 0;
    uint8_t tier_fallback_target = 0;  // vortex::Tier as u8
};

/// Region descriptor (docs/deopt-rbpd.md section 1).
struct RegionDescriptor {
    uint32_t region_id = 0;
    RegionKind kind = RegionKind::Hot;
    uint32_t entry_offset = 0;
    uint32_t exit_offset = 0;
    uint32_t bytecode_pc_begin = 0;
    uint32_t bytecode_pc_end = 0;
    uint8_t tier = 0;
    uint32_t failure_count = 0;
    PartialDeoptRecord deopt;
};

/// The region table of one compiled method (docs/ir-son-ciog.md section 5).
class RegionTable {
public:
    uint32_t add_region(RegionDescriptor desc);

    RegionDescriptor* find_by_pc_offset(uint32_t code_offset) noexcept;
    const RegionDescriptor* find_by_pc_offset(uint32_t code_offset) const noexcept {
        return const_cast<RegionTable*>(this)->find_by_pc_offset(code_offset);
    }

    /// Guard-failure protocol steps 4-6 (docs/deopt-rbpd.md section 4):
    /// increments the region's failure counter and returns the recovery path
    /// decision. M0 implements 3 of the spec's 5 paths: 0 = recompile region,
    /// 1 = successor region, 2 = tier fallback (shared trampoline + baseline
    /// fallback arrive with the J1 corpus, roadmap M1/M3).
    int on_guard_failure(uint32_t region_id, uint32_t failure_threshold = 16);

    std::span<const RegionDescriptor> regions() const noexcept { return regions_; }
    size_t size() const noexcept { return regions_.size(); }

private:
    std::vector<RegionDescriptor> regions_;
};

}  // namespace vortex::deopt
