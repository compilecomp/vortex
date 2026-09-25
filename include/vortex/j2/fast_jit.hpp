// J2 — fast optimizing JIT (docs/tier-j2.md, docs/roadmap.md M2).
//
// Pipeline: build_graph -> run_pipeline (stages 2-17) -> linear scan (18)
// -> instruction selection + peephole + emission (19-21). The emitted code
// executes against the J1 ABI (J1Context + J1EntryFn) so tiering T0 -> J1 ->
// J2 shares one runtime contract; J2's advantage is SSA register allocation,
// folding, value numbering, guard-optimized speculation, and bounded
// inlining.
//
// Deopt (Rules 39/40/42): every guard carries a complete FrameState. Guard
// failure enters an out-of-line stub that materializes the frame registers
// into the deopt window and traps into the runtime, which resumes T0 at the
// exact pc (Interpreter::resume) — no restart, no duplicated effects.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "vortex/infra/code_range.hpp"
#include "vortex/infra/security.hpp"
#include "vortex/j1/context.hpp"
#include "vortex/j1/baseline_jit.hpp"  // J1Bindings (shared runtime binding)
#include "vortex/j2/passes.hpp"        // PipelineStats (Rule 120 telemetry)
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::j2 {

using j1::J1EntryFn;
using j1::J1OsrFn;

/// Compilation task (the J2 mirror of j1::BaselineJob; tables are shared).
struct J2Job {
    const ugb::UGBModule* module = nullptr;
    uint32_t method_id = 0;
    const std::vector<ugb::ProfileSlot>* profiles = nullptr;
    const std::vector<ugb::IcSlot>* ics = nullptr;
    const std::vector<void*>* klass_addrs = nullptr;
    /// The heap's boxed-double klass (BoxedF64 guards; nullptr leaves the
    /// guard at the never-matching 0 sentinel = always deopt, still correct).
    const void* double_klass = nullptr;
    /// Node/pipeline budget (docs/tier-j2.md section 1).
    size_t node_cap = 20'000;
    /// Bitmask of DISABLED passes (Rule 59/131): bit i set = pass i off.
    /// 0 = every pass enabled. Bits follow the PassControl enum.
    uint64_t pass_kill_switches = 0;
};

/// One deopt frame of one guard record (Rule 42: complete FrameState).
struct DeoptFrame {
    uint32_t method_id = 0;
    uint32_t resume_pc = 0;   // guard pc; call-return pc for outer frames
    uint32_t inject_dst = 0xFFFFFFFFu;  // call continuation register
    uint32_t vreg_base = 0;   // index into the window capture
    uint32_t vreg_count = 0;
};

/// One guard's deopt metadata. Owned by the J2Code the emitted stubs point
/// into (stable addresses — the stubs embed them as immediates).
struct DeoptRecord {
    std::vector<DeoptFrame> frames;  // innermost first
};

/// Codegen artifact: code + compact metadata (J1-shape contracts).
///
/// Metadata formats (Rule 86: GC/deopt metadata is a required output):
///   gc_maps:        [count]{site u32, words u32, bits u32[words]} — per
///                   safepoint (Call/Allocate/Safepoint), `site` is the native
///                   offset of the call instruction; bit i = frame slot i
///                   (slot_disp numbering) holds a reference.
///   deopt_records:  [count]{len u32, frames u32,
///                            [frames]{method u32, pc u32, inject u32,
///                                     vreg_base u32, vreg_count u32}}
///                   innermost frame first; serialized copy of `records` for
///                   replay/inspection (Rule 128) — the live structs the
///                   stubs reference ride in `records`.
struct J2Code {
    uint32_t method_id = 0;
    std::vector<uint8_t> code;
    uint32_t entry_offset = 0;
    uint32_t osr_entry_offset = 0xFFFFFFFFu;
    std::vector<uint32_t> osr_pcs;
    std::vector<uint8_t> gc_maps;
    std::vector<uint8_t> deopt_records;
    /// Live deopt records — the emitted stubs embed addresses INTO this
    /// vector, so it is shared (never copied) from compilation through
    /// publication. Stable addresses: reserved once, filled once.
    std::shared_ptr<std::vector<DeoptRecord>> records;
    bool budget_exceeded = false;  // pipeline stopped early; code still valid
    /// Pass telemetry (Rule 120: deterministic, assertable in tests —
    /// golden pass counts lock the pipeline's behavior).
    PipelineStats stats;
};

/// A published (executable) J2 method. Owns the code memory and the deopt
/// records the stubs reference.
struct J2Executable {
    infra::WritableCodeMemory memory;
    J1EntryFn entry = nullptr;
    J1OsrFn osr_entry = nullptr;
    uint32_t method_id = 0;
    std::shared_ptr<const std::vector<DeoptRecord>> records;
};

/// Compiles one method through the full J2 pipeline. Refusal (unsupported
/// opcode, budget) fails the Result with a named reason — the caller keeps
/// the method on J1/T0 (Rule 11/76).
/// Debug tracing for the deopt path (drivers/tests; production leaves it
/// off — the hook is a rare-event path, the flag is read there only).
void set_j2_trace(bool enabled) noexcept;

support::Result<J2Code> compile_j2(const J2Job& job);

/// W^X publication (the shared code range keeps J2 code within rel32 of the
/// LDPT arenas and the tier stack).
support::Result<J2Executable> publish_j2(const J2Code& code,
                                         infra::CodeRange* range = nullptr);

/// Runs a published method end-to-end against the shared J1 bindings.
/// kErrDeopt exits chain through Interpreter::resume with the captured
/// frames (state-exact continuation, Rules 39/40/42); a deopt id WITHOUT a
/// capture (safepoint poll) reruns the whole method in T0 — the M1
/// suspension contract, kept for parity.
support::Result<TaggedValue> run_j2(J2Executable& ex, j1::J1Bindings& bindings,
                                    std::span<const TaggedValue> args,
                                    vm::Interpreter& interp);

}  // namespace vortex::j2
