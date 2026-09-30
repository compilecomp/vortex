// RBPD — Region-Based Partial Deoptimization (docs/deopt-rbpd.md).
//
// M3 implements the full deopt core: region descriptors, escape sets,
// partial deopt records, continuation stubs, failure counters with the
// Rule-43 throttle, the 5-path recovery protocol, region merge/split, and
// the partial-recompile driver contract the J3 tiering driver calls.
//
// Laws implemented here (Compiler Laws):
//   Rule 39 — a partial deopt reconstructs state observationally
//             indistinguishable from the lower tier at the resume pc; the
//             escape set defines exactly what must be materialized.
//   Rule 40 — full deopt to T0 stays reachable: every region record names
//             tier_fallback_target, and the engine falls back whenever a
//             region cannot be recovered.
//   Rule 41 — effects before a failing guard are never rolled back; the
//             escape set carries every value the continuation needs, so
//             recovery re-enters AFTER committed effects only.
//   Rule 43 — deopt loops are throttled: per-site, per-region and
//             per-method counters with reasons and tier history; chronic
//             failure disables the speculation, downgrades the tier, or
//             blacklists the region.
//   Rule 113 — frames are reconstructible on demand: the escape locations
//             are the (vreg -> REG|STACK|CONST) map the continuation stub
//             materializes.
#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
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

/// Region lifecycle (docs/deopt-rbpd.md section 5: partial recompile marks
/// the old region stale; the replacement is the recompiled one).
enum class RegionState : uint8_t {
    Live,        // callable; failures are counted
    Stale,       // superseded; call sites were patched away — never re-enter
    Recompiled,  // this descriptor replaced an earlier stale one
    Blacklisted, // Rule 43: chronic failure — the tiering driver consults
                 // throttle_verdict and refuses further speculation here;
                 // the entry-trap enforcement lands with the J4 tiering
                 // driver (M3 records the state; M4 consumes it)
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
    RegionState state = RegionState::Live;
    uint32_t entry_offset = 0;
    uint32_t exit_offset = 0;
    uint32_t bytecode_pc_begin = 0;
    uint32_t bytecode_pc_end = 0;
    uint8_t tier = 0;
    uint32_t failure_count = 0;
    PartialDeoptRecord deopt;
};

/// The escape set of one region (docs/deopt-rbpd.md section 2): the values
/// the continuation needs — nothing else is materialized during partial
/// deopt, which is what makes it cheap.
struct EscapeSet {
    /// Values consumed outside the region / live across its exit / needed
    /// by the continuation stub. GC roots are the reference-typed subset
    /// (the GC map is the root view; Rule 78 keeps it mapped).
    std::vector<EscapeLocation> live_across_exit;
    /// Bytecode register file positions each escaped value lands in for the
    /// T0 continuation (indexed parallel to live_across_exit when the
    /// continuation is an interpreter resume; empty otherwise).
    std::vector<uint32_t> continuation_slots;
    uint32_t bytecode_resume_pc = 0;

    size_t size() const noexcept { return live_across_exit.size(); }
};

/// Recovery paths (docs/deopt-rbpd.md section 4). The int-valued 0/1/2
/// order matches the M0 protocol and existing callers.
enum class RecoveryPath : uint8_t {
    RecompileRegion = 0,     // A: recompile only this region
    SuccessorRegion = 1,     // B: jump to the successor region
    TierFallback = 2,        // C/D: baseline / interpreter fallback
    SharedTrampoline = 3,    // E: shared deopt trampoline
};

/// Why a region failed (Rule 43: the reason is recorded, not guessed).
enum class DeoptReason : uint8_t {
    GuardFailed = 0,
    SpeculationViolated,
    SuspensionPoll,
    BudgetWatchdog,
    ChronicFailure,
};

/// One deopt event (Rule 43: counts + reason + tier history + tick window).
struct DeoptEvent {
    uint32_t site_id = 0;        // guard/poll site identity (Rule 8)
    uint32_t region_id = 0;
    DeoptReason reason = DeoptReason::GuardFailed;
    uint8_t tier = 0;
    uint64_t tick = 0;           // caller-provided monotonic tick (the
                                 // engine stays clock-free and testable)
};

/// Per-compiled-method deopt accounting (Rule 43). Thresholds:
///   site_threshold     — same site failing repeatedly disables the site's
///                        speculation (recompile with weaker assumptions);
///   region_threshold   — a region past it falls back below this tier;
///   method_threshold   — the whole method is blacklisted for a while
///                        (tier history records the demotion).
struct ThrottlePolicy {
    uint32_t site_threshold = 8;
    uint32_t region_threshold = 16;
    uint32_t method_threshold = 64;
};

/// Rule-43 consumption (docs/deopt-rbpd.md section 9; roadmap M4): the
/// ENTRY-TRAP decision the run path and the tiering driver must honor
/// BEFORE entering or continuing in optimized code. The ladder mirrors
/// throttle_verdict's numbers so the two never disagree.
enum class EntryDecision : uint8_t {
    Enter = 0,              // speculation healthy — enter
    WeakenAssumptions = 1,  // a site is past its threshold — entering is
                            // still legal, but the driver must schedule a
                            // weaker-assumption recompile
    DowngradeTier = 2,      // the target region is dead (Stale /
                            // Blacklisted) or some region is past its
                            // threshold — refuse entry; the driver moves
                            // the method to its fallback tier (Rule 40)
    RefuseMethod = 3,       // method-wide chronic failure — no speculation
                            // on this method until the caller resets expiry
                            // (it owns the reset)
};

/// Named form of the entry-trap decision (Rule 76: refusals carry the
/// reason; Rule 120: telemetry is spellable).
constexpr const char* entry_decision_name(EntryDecision d) noexcept {
    switch (d) {
        case EntryDecision::Enter: return "Enter";
        case EntryDecision::WeakenAssumptions: return "WeakenAssumptions";
        case EntryDecision::DowngradeTier: return "DowngradeTier";
        case EntryDecision::RefuseMethod: return "RefuseMethod";
    }
    return "Unknown";
}

class RegionTable {
public:
    uint32_t add_region(RegionDescriptor desc);

    RegionDescriptor* find_by_pc_offset(uint32_t code_offset) noexcept;
    const RegionDescriptor* find_by_pc_offset(uint32_t code_offset) const noexcept {
        return const_cast<RegionTable*>(this)->find_by_pc_offset(code_offset);
    }
    RegionDescriptor* find_by_id(uint32_t region_id) noexcept;
    const RegionDescriptor* find_by_id(uint32_t region_id) const noexcept {
        return const_cast<RegionTable*>(this)->find_by_id(region_id);
    }

    /// Guard-failure protocol steps 4-6 (docs/deopt-rbpd.md section 4):
    /// increments ONLY the failing region's counter, records the event
    /// (Rule 43), and returns the recovery path decision. The legacy
    /// two-argument form keeps the M0 contract (0 = recompile region,
    /// 1 = successor, 2 = tier fallback) with the default policy.
    RecoveryPath on_guard_failure(uint32_t region_id,
                                  const ThrottlePolicy& policy,
                                  uint32_t site_id, DeoptReason reason,
                                  uint64_t tick);
    int on_guard_failure(uint32_t region_id, uint32_t failure_threshold);

    /// Suspension accounting (poll fired): recorded as an event but NEVER
    /// as a failure — suspension is not speculation loss (M1 contract),
    /// so counters stay untouched.
    void on_suspension(uint32_t region_id, uint32_t site_id, uint64_t tick);

    /// Rule 43 decision for one method after the counters moved: 0 = keep
    /// running, 1 = recompile with weaker assumptions, 2 = downgrade tier,
    /// 3 = blacklist (temporary; the caller owns expiry).
    int throttle_verdict(const ThrottlePolicy& policy) const;

    /// The entry trap (EntryDecision above). `target_region` narrows the decision to one region
    /// (UINT32_MAX = whole-method entry): a dead target refuses regardless
    /// of how healthy the rest of the method is, because the caller asked
    /// to enter THAT region. An executable with no region table records
    /// (M0 legacy shape) always enters — its fallback is the Rule-40
    /// whole-method rerun by construction.
    EntryDecision entry_check(const ThrottlePolicy& policy,
                              uint32_t target_region = UINT32_MAX) const;

    const DeoptEvent* last_event() const noexcept { return &last_event_; }
    uint64_t method_failure_count() const noexcept {
        return method_failure_count_;
    }
    /// Per-site counts (Rule 43: deopt count per site).
    uint64_t site_failure_count(uint32_t site_id) const;

    /// Region management (docs/deopt-rbpd.md section 6): adjacent regions
    /// that keep failing together merge; one failing guard in a large
    /// region splits around it; tiny/large regions rebalance.
    uint32_t merge_regions(uint32_t a_id, uint32_t b_id);
    /// Splits `region_id` at `split_code_offset`; returns the new region's
    /// id (the tail) or UINT32_MAX when the split is not applicable.
    uint32_t split_region(uint32_t region_id, uint32_t split_code_offset);
    /// Partial recompile bookkeeping (section 5): mark old stale, register
    /// the replacement, return the new id. Call sites patch through the
    /// dependency engine; the table only tracks the lifecycle.
    uint32_t replace_region(uint32_t old_id, RegionDescriptor replacement);

    std::span<const RegionDescriptor> regions() const noexcept { return regions_; }
    size_t size() const noexcept { return regions_.size(); }
    size_t live_size() const noexcept;

private:
    std::vector<RegionDescriptor> regions_;
    std::unordered_map<uint32_t, uint64_t> site_counts_;
    DeoptEvent last_event_;
    uint64_t method_failure_count_ = 0;
};

}  // namespace vortex::deopt
