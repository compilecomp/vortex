// Infra 9 — Power & Thermal Management
// (docs/infrastructure/09-power-thermal.md).
//
// On thermal pressure or battery saver the governor suspends J4 compilation,
// downgrades J3 budgets, shrinks the M:N worker pool, and disables aggressive
// prefetching. M0 ships the policy model; the OS thermal listener is a
// polling adapter stub.
#pragma once

#include <cstdint>

namespace vortex::infra {

enum class PowerState : uint8_t { Normal, BatterySaver, ThermalWarm, ThermalCritical };

/// What the governor computes from a power state; consumed by the tiering
/// policy and the compile queue.
struct GovernorAction {
    bool suspend_j4 = false;
    double j3_budget_multiplier = 1.0;
    uint32_t worker_pool_shrink = 0;
    bool disable_prefetch = false;
};

class PowerGovernor {
public:
    static GovernorAction apply(PowerState state) noexcept;
};

/// OS thermal status source (polling adapter). Contract stub — the interface
/// is fixed so platform adapters can plug in.
class ThermalListener {
public:
    virtual ~ThermalListener() = default;
    virtual PowerState current_state() const noexcept = 0;
};

/// Default listener that always reports Normal (desktop/CI).
class FixedPowerListener final : public ThermalListener {
public:
    explicit FixedPowerListener(PowerState state = PowerState::Normal) noexcept
        : state_(state) {}
    PowerState current_state() const noexcept override { return state_; }

private:
    PowerState state_;
};

}  // namespace vortex::infra
