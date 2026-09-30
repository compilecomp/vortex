// Deterministic tiering policy (docs/architecture.md section 1.3, docs/tier-j4.md
// section 10). Thresholds are explicit configuration; transitions are edge-
// triggered events observed by compile-queue owners.
#pragma once

#include <cstdint>

#include "vortex/deopt/rbpd.hpp"
#include "vortex/support/tier.hpp"

namespace vortex {

/// Thresholds for upward promotion. Defaults follow the graduation model:
/// J1 needs hotness only; J2 basic stability; J3 stable ICs; J4 phase stability
/// with low deopt rate.
struct TieringThresholds {
    uint32_t j1_invocations = 100;
    uint32_t j1_backedges = 1'000;
    uint32_t j2_invocations = 1'000;
    uint32_t j2_backedges = 10'000;
    uint32_t j3_invocations = 10'000;
    uint32_t j3_backedges = 100'000;
    uint32_t j4_invocations = 100'000;
    double j4_max_deopt_rate = 0.001;
};

/// Observes tier-transition decisions. The compile queue subsystem subscribes;
/// in M0 the observer records events for the `vx stats` sink.
class TieringObserver {
public:
    virtual ~TieringObserver() = default;
    virtual void on_promote(uint32_t method_id, Tier from, Tier to) = 0;
    virtual void on_demote(uint32_t method_id, Tier from, Tier to) = 0;
    virtual void on_osr_request(uint32_t method_id, uint32_t bytecode_pc,
                                Tier target) = 0;
};

/// A no-op observer used when no compile queue is installed.
class NullTieringObserver final : public TieringObserver {
public:
    void on_promote(uint32_t, Tier, Tier) override {}
    void on_demote(uint32_t, Tier, Tier) override {}
    void on_osr_request(uint32_t, uint32_t, Tier) override {}
};

/// Per-method hotness state maintained by the T0 interpreter and lower tiers.
struct MethodHotness {
    uint32_t invocations = 0;
    uint32_t backedges = 0;
    uint32_t deopts = 0;
    Tier current = Tier::T0;
};

/// The deterministic policy: pure function of (hotness, thresholds).
class TieringPolicy {
public:
    explicit TieringPolicy(TieringThresholds thresholds = {}) noexcept
        : thresholds_(thresholds) {}

    /// Returns the tier the method should be on given its hotness, or the
    /// current tier if no transition should fire.
    ///
    /// Rule-43 consumption (docs/tier-j4.md section 12.4; roadmap M4):
    /// `speculation` is the method's RBPD entry verdict (Enter when the
    /// method has no optimized code or a healthy region table). A method
    /// past the throttle ladder is NOT re-speculated: escalations to
    /// J2/J3/J4 are refused while J1 (the non-speculative stencil) stays
    /// reachable, and a method already on an optimized tier falls back to
    /// T0 (Rule 40) until the gate owner clears the block. The caller
    /// records the transition (Rule 28) — the policy stays a pure function.
    Tier evaluate(const MethodHotness& h,
                  deopt::EntryDecision speculation =
                      deopt::EntryDecision::Enter) const noexcept {
        const bool blocked =
            speculation == deopt::EntryDecision::DowngradeTier ||
            speculation == deopt::EntryDecision::RefuseMethod;
        if (blocked && h.current >= Tier::J2) {
            return Tier::T0;  // Rule 40: demote to the fallback tier
        }
        if (h.current < Tier::J1 && h.invocations >= thresholds_.j1_invocations) {
            return Tier::J1;
        }
        if (!blocked) {
            if (h.current < Tier::J2 && h.invocations >= thresholds_.j2_invocations) {
                return Tier::J2;
            }
            if (h.current < Tier::J3 && h.invocations >= thresholds_.j3_invocations) {
                return Tier::J3;
            }
            if (h.current < Tier::J4 && h.invocations >= thresholds_.j4_invocations) {
                const double rate =
                    h.invocations > 0 ? static_cast<double>(h.deopts) / h.invocations : 0.0;
                if (rate <= thresholds_.j4_max_deopt_rate) return Tier::J4;
            }
        }
        return h.current;
    }

    bool should_osr(const MethodHotness& h) const noexcept {
        return h.backedges >= thresholds_.j1_backedges;
    }

    const TieringThresholds& thresholds() const noexcept { return thresholds_; }
    void set_observer(TieringObserver* observer) noexcept { observer_ = observer; }
    TieringObserver* observer() const noexcept { return observer_; }

private:
    TieringThresholds thresholds_;
    TieringObserver* observer_ = nullptr;
};

}  // namespace vortex
