#include "vortex/deopt/rbpd.hpp"

namespace vortex::deopt {

uint32_t RegionTable::add_region(RegionDescriptor desc) {
    const uint32_t id = static_cast<uint32_t>(regions_.size());
    desc.region_id = id;
    regions_.push_back(std::move(desc));
    return id;
}

RegionDescriptor* RegionTable::find_by_pc_offset(uint32_t code_offset) noexcept {
    for (auto& r : regions_) {
        // Never re-enter stale (superseded) or blacklisted (Rule 43: entry
        // traps to the fallback tier) regions.
        if (r.state == RegionState::Stale ||
            r.state == RegionState::Blacklisted) {
            continue;
        }
        if (code_offset >= r.entry_offset && code_offset < r.exit_offset) {
            return &r;
        }
    }
    return nullptr;
}

RegionDescriptor* RegionTable::find_by_id(uint32_t region_id) noexcept {
    return region_id < regions_.size() ? &regions_[region_id] : nullptr;
}

uint64_t RegionTable::site_failure_count(uint32_t site_id) const {
    auto it = site_counts_.find(site_id);
    return it != site_counts_.end() ? it->second : 0;
}

RecoveryPath RegionTable::on_guard_failure(uint32_t region_id,
                                           const ThrottlePolicy& policy,
                                           uint32_t site_id,
                                           DeoptReason reason, uint64_t tick) {
    if (region_id >= regions_.size()) return RecoveryPath::TierFallback;
    RegionDescriptor& region = regions_[region_id];
    // Protocol step 4 (docs/deopt-rbpd.md section 4): count the failure —
    // ONLY the failing region advances. Neighboring regions are untouched
    // (the isolation property the DoD test pins).
    ++region.failure_count;
    ++method_failure_count_;
    ++site_counts_[site_id];
    last_event_ = DeoptEvent{site_id, region_id, reason, region.tier, tick};

    // Rule 43: chronic failure is decided on the counts — region-level
    // first (the stronger signal), then the site's.
    const uint64_t site_n = site_failure_count(site_id);
    if (method_failure_count_ >= policy.method_threshold ||
        region.failure_count >= policy.region_threshold) {
        region.state = RegionState::Blacklisted;
        return RecoveryPath::TierFallback;
    }
    if (site_n >= policy.site_threshold) {
        // Same site again and again: the site's speculation dies (recompile
        // with weaker assumptions is the caller's action); the region still
        // recovers through its successor when one exists.
        if (region.deopt.successor_region_id != 0) {
            return RecoveryPath::SuccessorRegion;
        }
        return RecoveryPath::RecompileRegion;
    }
    // Protocol step 6: below thresholds, prefer the successor region (jump
    // forward past the failed speculation), else recompile the region.
    if (region.deopt.successor_region_id != 0 &&
        region.deopt.successor_region_id < regions_.size()) {
        return RecoveryPath::SuccessorRegion;
    }
    return RecoveryPath::RecompileRegion;
}

int RegionTable::on_guard_failure(uint32_t region_id,
                                  uint32_t failure_threshold) {
    ThrottlePolicy policy;
    policy.region_threshold = failure_threshold;
    // Legacy M0 contract: the REGION counter is the only signal (no site
    // tracking, no method-wide blacklisting from this form).
    policy.site_threshold = UINT32_MAX;
    policy.method_threshold = UINT32_MAX;
    const RecoveryPath path = on_guard_failure(
        region_id, policy, /*site_id=*/0, DeoptReason::GuardFailed,
        /*tick=*/method_failure_count_);
    return static_cast<int>(path);
}

int RegionTable::throttle_verdict(const ThrottlePolicy& policy) const {
    if (method_failure_count_ >= policy.method_threshold) return 3;  // blacklist
    for (const RegionDescriptor& r : regions_) {
        if (r.state == RegionState::Blacklisted) return 2;  // downgrade tier
        if (r.failure_count >= policy.region_threshold) return 2;
    }
    for (const auto& [site, count] : site_counts_) {
        (void)site;
        if (count >= policy.site_threshold) return 1;  // weaker assumptions
    }
    return 0;
}

void RegionTable::on_suspension(uint32_t region_id, uint32_t site_id,
                                uint64_t tick) {
    // The event carries the RAW region id (an out-of-range id stays out of
    // range — clamping it onto region 0 would be a telemetry lie, R127).
    last_event_ = DeoptEvent{
        site_id, region_id, DeoptReason::SuspensionPoll,
        region_id < regions_.size() ? static_cast<uint8_t>(regions_[region_id].tier)
                                    : static_cast<uint8_t>(0),
        tick};
}

uint32_t RegionTable::merge_regions(uint32_t a_id, uint32_t b_id) {
    if (a_id >= regions_.size() || b_id >= regions_.size() || a_id == b_id) {
        return UINT32_MAX;
    }
    RegionDescriptor& a = regions_[a_id];
    RegionDescriptor& b = regions_[b_id];
    if (a.state != RegionState::Live || b.state != RegionState::Live) {
        return UINT32_MAX;
    }
    // Adjacent in code: [a.entry, b.exit) with a.exit == b.entry.
    if (a.exit_offset != b.entry_offset) return UINT32_MAX;
    a.exit_offset = b.exit_offset;
    a.bytecode_pc_end = b.bytecode_pc_end;
    a.failure_count += b.failure_count;
    a.deopt.bytecode_resume_pc = b.deopt.bytecode_resume_pc;
    a.deopt.escape_locations = std::move(b.deopt.escape_locations);
    a.deopt.successor_region_id = b.deopt.successor_region_id;
    a.deopt.tier_fallback_target = b.deopt.tier_fallback_target;
    b.state = RegionState::Stale;
    return a_id;
}

uint32_t RegionTable::split_region(uint32_t region_id,
                                   uint32_t split_code_offset) {
    if (region_id >= regions_.size()) return UINT32_MAX;
    RegionDescriptor& r = regions_[region_id];
    if (r.state != RegionState::Live) return UINT32_MAX;
    if (split_code_offset <= r.entry_offset ||
        split_code_offset >= r.exit_offset) {
        return UINT32_MAX;
    }
    RegionDescriptor tail = r;  // same kind/tier/record shape
    tail.region_id = static_cast<uint32_t>(regions_.size());
    tail.entry_offset = split_code_offset;
    tail.failure_count = 0;
    tail.deopt.region_id = tail.region_id;
    r.exit_offset = split_code_offset;
    r.deopt.successor_region_id = tail.region_id;
    regions_.push_back(std::move(tail));
    return regions_.back().region_id;
}

uint32_t RegionTable::replace_region(uint32_t old_id,
                                     RegionDescriptor replacement) {
    if (old_id >= regions_.size()) return UINT32_MAX;
    RegionDescriptor& old_r = regions_[old_id];
    // The replacement inherits the pc range and code placement contract of
    // the region it supersedes (the recompiler re-derives its own record).
    replacement.bytecode_pc_begin = old_r.bytecode_pc_begin;
    replacement.bytecode_pc_end = old_r.bytecode_pc_end;
    replacement.kind = old_r.kind;
    replacement.tier = old_r.tier;
    replacement.region_id = static_cast<uint32_t>(regions_.size());
    old_r.state = RegionState::Stale;
    replacement.state = RegionState::Recompiled;
    regions_.push_back(std::move(replacement));
    return regions_.back().region_id;
}

size_t RegionTable::live_size() const noexcept {
    size_t n = 0;
    for (const RegionDescriptor& r : regions_) {
        if (r.state == RegionState::Live) ++n;
    }
    return n;
}

}  // namespace vortex::deopt
