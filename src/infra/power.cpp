#include "vortex/infra/power.hpp"

namespace vortex::infra {

GovernorAction PowerGovernor::apply(PowerState state) noexcept {
    GovernorAction action;
    switch (state) {
    case PowerState::Normal:
        break;
    case PowerState::BatterySaver:
        action.suspend_j4 = true;
        action.j3_budget_multiplier = 0.5;
        action.worker_pool_shrink = 1;
        action.disable_prefetch = true;
        break;
    case PowerState::ThermalWarm:
        action.suspend_j4 = true;
        action.j3_budget_multiplier = 0.5;
        break;
    case PowerState::ThermalCritical:
        action.suspend_j4 = true;
        action.j3_budget_multiplier = 0.25;
        action.worker_pool_shrink = 2;
        action.disable_prefetch = true;
        break;
    }
    return action;
}

}  // namespace vortex::infra
