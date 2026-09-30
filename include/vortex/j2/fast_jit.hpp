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
#include "vortex/deopt/rbpd.hpp"
#include "vortex/ir/escape_summary.hpp"
#include "vortex/j1/context.hpp"
#include "vortex/j1/baseline_jit.hpp"  // J1Bindings (shared runtime binding)
#include "vortex/j2/passes.hpp"        // PipelineStats (Rule 120 telemetry)
#include "vortex/runtime/interop.hpp"  // POLY_* lowering license
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
    /// The interop registry (docs/interop-protocol.md): the POLY_* lowering
    /// license source. Null = POLY_* stays T0-only (named refusal).
    const runtime::interop::InteropRegistry* interop = nullptr;
};

/// One deopt frame of one guard record (Rule 42: complete FrameState).
struct DeoptFrame {
    uint32_t method_id = 0;
    uint32_t resume_pc = 0;   // guard pc; call-return pc for outer frames
    uint32_t inject_dst = 0xFFFFFFFFu;  // call continuation register
    uint32_t vreg_base = 0;   // index into the window capture
    uint32_t vreg_count = 0;
    /// Rematerialization descriptors for scalar-replaced allocations in
    /// this frame (XLEA phase 2 deopt contract, docs/xlea.md section 2):
    /// the runtime rebuilds each object from the window's field values
    /// before resuming T0 — state-exact (Rule 39). The serialized
    /// deopt_records blob (replay/inspection) does not carry remats; the
    /// live structs the stubs reference do.
    struct RematEntry {
        uint32_t slot = 0;        // window slot carrying the undefined word
        uint32_t klass_token = 0;
        uint32_t field_count = 0;
        /// Field sources, parallel: window slots of live field values, or
        /// kConstSlot (UINT32_MAX) with the word embedded in const_words
        /// (constants rematerialize without a window slot).
        static constexpr uint32_t kConstSlot = 0xFFFFFFFFu;
        /// Parallel to field_slots/const_words: the klass FIELD SLOT each
        /// entry writes (positional fills would swap fields when the store
        /// order differs from slot order).
        std::vector<uint32_t> field_keys;
        std::vector<uint32_t> field_slots;
        std::vector<uint64_t> const_words;
    };
    std::vector<RematEntry> remats;
};

/// One guard's deopt metadata. Owned by the J2Code; the emitted stubs
/// reference records through publish-time relocations (Rule 56: the
/// DEOPT-RECORD references are address-free until publication; the
/// remaining baked addresses — klass handles passed via the job — are
/// compile-time inputs shared by every repeated compile of one runtime
/// instance, so they do not affect the same-instance determinism DoD).
struct DeoptRecord {
    std::vector<DeoptFrame> frames;  // innermost first
    /// RBPD identity: region_id == the index of this record's region in the
    /// compiled method's RegionTable; suspension marks poll-fired records
    /// (accounted as suspension events, not guard failures — Rule 43 counts
    /// them separately).
    uint32_t region_id = 0;
    bool suspension = false;
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
    /// RBPD region table (docs/deopt-rbpd.md section 1): one region per
    /// deopt record — every trap site aligns with a region boundary (the
    /// hard invariant). Failure counters live here at runtime.
    deopt::RegionTable regions;
    /// Stage-29 output (J3): the escape summary of the compiled body,
    /// bound to the post-pipeline graph hash (docs/xlea.md section 4.1).
    /// J2 leaves has_summary = false.
    ir::EscapeSummary summary;
    bool has_summary = false;
    /// Live deopt records — shared (never copied) from compilation through
    /// publication; publish_j2 patches the stub relocations against THIS
    /// vector. Stable addresses: reserved once, filled once.
    std::shared_ptr<std::vector<DeoptRecord>> records;
    /// Publish-time address patches (Rule 56 determinism): the stubs'
    /// record-pointer imm64s carry ZERO in `code`; publish_j2 patches each
    /// {code_offset} with &records[record_index]. The deopt-record
    /// references are therefore address-free and the compile output is
    /// bit-identical across repeated compiles of one runtime instance
    /// (klass-handle imm64s are compile-time inputs, see DeoptRecord
    /// above); the runtime behavior is unchanged (same addresses, applied
    /// later).
    struct RecordReloc {
        uint32_t code_offset = 0;   // offset of the imm64 payload in code
        uint32_t record_index = 0;  // index into `records`
    };
    std::vector<RecordReloc> record_relocs;
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
    /// Mutable: the runtime advances failure counters on it (Rule 43).
    std::shared_ptr<deopt::RegionTable> regions;
    /// The Rule-43 ladder this executable is judged against (entry trap +
    /// failure accounting use the SAME policy so the two never disagree).
    /// The tiering driver may tighten it per method (docs/deopt-rbpd.md 7).
    deopt::ThrottlePolicy throttle_policy;
};

/// Deopt chain depth bound (the pipeline's inline_depth_cap + root,
/// rounded up; a defensive named bound, not a tuning knob — Rule 72).
/// SHARED with the J4 engine: J4's inline depth is capped at
/// `kMaxDeoptFrames - 1` so every spliced frame stays representable in
/// the deopt record (a runtime-ABI capacity constraint — a Rule-15 safety
/// constraint, not an optimization budget).
inline constexpr size_t kMaxDeoptFrames = 8;

/// Compiles one method through the full J2 pipeline. Refusal (unsupported
/// opcode, budget) fails the Result with a named reason — the caller keeps
/// the method on J1/T0 (Rule 11/76).
/// Debug tracing for the deopt path (drivers/tests; production leaves it
/// off — the hook is a rare-event path, the flag is read there only).
void set_j2_trace(bool enabled) noexcept;

support::Result<J2Code> compile_j2(const J2Job& job);

/// Shared optimizing backend (J2 and J3): dominance placement, profile
/// layout, the emission plan, linear scan, instruction selection and
/// emission. `built` must have completed its tier's pass pipeline; `stats`
/// rides into J2Code for telemetry (Rule 120). The region table is built
/// under `tier` (the RBPD descriptors name their owning tier).
support::Result<J2Code> emit_optimized(const J2Job& job, BuiltGraph& built,
                                       const PipelineStats& stats,
                                       Tier tier = Tier::J2);

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
