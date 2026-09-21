// UGB verifier (docs/ugb.md section 9). Verification is mandatory before
// execution — the `vx run` path refuses unverified modules.
#pragma once

#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"
namespace vortex::ugb {

using support::Result;

/// Verifies a module: opcode validity, register bounds, call argument
/// windows, token ranges, branch-target validity, and terminator presence.
/// This is the M0 subset of ugb.md section 9's twelve mandatory checks;
/// GC reference safety, capability satisfaction, and suspend/deopt state
/// consistency are enforced when those subsystems activate (roadmap M3/M5).
Result<void> verify_module(const UGBModule& module);

/// Per-method verifier used by tests.
Result<void> verify_method(const UGBModule& module, const UGBMethod& method);

}  // namespace vortex::ugb
