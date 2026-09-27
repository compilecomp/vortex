// J3 — adaptive full optimizing JIT (docs/tier-j3.md): the budgeted full
// pipeline (60 stages, three waves — j3/passes.hpp), full RBPD region
// capture (deopt/rbpd.hpp), the CIOG overlay and XLEA escape summaries.
//
// Execution contract: identical to J2 — the emitted code runs on the J1
// ABI against the shared code range, tiering hands off T0 -> J1 -> J2 ->
// J3. J3's advantage is the deeper pipeline: redundancy elimination,
// ranges, EA/scalar replacement (XLEA), LICM, and the RBPD region table
// with per-region failure accounting (Rule 43).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "vortex/deopt/rbpd.hpp"
#include "vortex/ir/escape_summary.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j1/context.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/j3/passes.hpp"
#include "vortex/runtime/interop.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::j3 {

using j2::J2Code;
using j2::J2Job;

/// A published (executable) J3 method: the shared J2 executable shape plus
/// the escape summary the method publishes (stage 29; callers consume it
/// instead of re-analyzing — docs/xlea.md section 4.2).
struct J3Executable {
    j2::J2Executable core;
    ir::EscapeSummary summary;
};

/// Compiles one method through the full J3 pipeline. Refusal (unsupported
/// opcode, budget) fails the Result with a named reason — the method stays
/// on J2/J1/T0 (Rules 11/76).
support::Result<J2Code> compile_j3(const J2Job& job);

/// W^X publication + summary attach.
support::Result<J3Executable> publish_j3(const J2Code& code,
                                         const ir::EscapeSummary& summary,
                                         infra::CodeRange* range = nullptr);

/// Runs a published J3 method end-to-end (the J2 run contract: captured
/// deopt resumes state-exact via Interpreter::resume; guard failures
/// advance ONLY the failing region's counter — Rule 43; suspension polls
/// are recorded as suspension events, never failures).
support::Result<TaggedValue> run_j3(J3Executable& ex,
                                    j1::J1Bindings& bindings,
                                    std::span<const TaggedValue> args,
                                    vm::Interpreter& interp);

}  // namespace vortex::j3
