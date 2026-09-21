#include "vortex/deopt/rbpd.hpp"

namespace vortex::deopt {

uint32_t RegionTable::add_region(RegionDescriptor desc) {
    desc.region_id = static_cast<uint32_t>(regions_.size());
    regions_.push_back(std::move(desc));
    return desc.region_id;
}

RegionDescriptor* RegionTable::find_by_pc_offset(uint32_t code_offset) noexcept {
    for (auto& r : regions_) {
        if (code_offset >= r.entry_offset && code_offset < r.exit_offset) {
            return &r;
        }
    }
    return nullptr;
}

int RegionTable::on_guard_failure(uint32_t region_id,
                                  uint32_t failure_threshold) {
    if (region_id >= regions_.size()) return 2;
    auto& region = regions_[region_id];
    ++region.failure_count;
    // Recovery decision (docs/deopt-rbpd.md section 4): below threshold the
    // region is recompiled; at/after it the failure is chronic and we fall
    // through to the successor region or a lower tier.
    if (region.failure_count < failure_threshold) {
        if (region.deopt.successor_region_id != 0) return 1;
        return 0;
    }
    return 2;
}

}  // namespace vortex::deopt
