// J1 — no-IR stencil baseline JIT (docs/tier-j1.md, docs/roadmap.md M1).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "vortex/codegen/x64/assembler.hpp"
#include "vortex/gc/icggc.hpp"
#include "vortex/infra/security.hpp"
#include "vortex/j1/context.hpp"
#include "vortex/j1/stencil.hpp"
#include "vortex/runtime/object_model.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"
#include "vortex/vm/interpreter.hpp"

namespace vortex::j1 {

/// Compilation task: one method, from the bottom of the tier stack. The
/// klass/field tables come from the engine binding (make_j1_bindings) —
/// they may be T0-prepared (module.runtime) or freshly built.
struct BaselineJob {
    const ugb::UGBModule* module = nullptr;
    uint32_t method_id = 0;
    /// Class token -> Klass* (null entry = unresolved; guards stay at the
    /// never-matching 0 sentinel and take the slow path).
    const std::vector<void*>* klass_addrs = nullptr;
    /// Field token -> byte offset (16 + 8*slot); negative = unresolved.
    const int32_t* field_offsets = nullptr;
    size_t field_offset_count = 0;
    /// The heap's boxed-double klass (guards every F64 stencil; nullptr
    /// leaves the guard at the never-matching 0 sentinel = always slow).
    const void* double_klass = nullptr;
    /// Per-instruction IC profiles (docs/tier-t0.md section 3), index-aligned
    /// with the method's decode order. When a field-access site is
    /// Monomorphic, the instantiation strengthens the always-slow guard into
    /// a klass-checked fast path (docs/tier-j1.md section 7, roadmap M2).
    /// Null entries (or a null pointer) keep the M1 always-slow default —
    /// correct for every receiver.
    const ugb::IcSlot* ic_slots = nullptr;
    size_t ic_slot_count = 0;
};

/// Compiled baseline artifact: code + compact metadata
/// (docs/tier-j1.md sections 8-9).
struct BaselineCode {
    uint32_t method_id = 0;
    std::vector<uint8_t> code;           // ready for W^X publication
    uint32_t entry_offset = 0;           // always 0 (method entry)
    uint32_t osr_entry_offset = 0;       // 0xFFFFFFFF = no OSR stubs
    std::vector<uint32_t> osr_entries;   // bytecode PCs with OSR stubs
    std::vector<uint8_t> gc_maps;        // [count]{pc u32, bytes u32, map...}
    std::vector<uint8_t> deopt_records;  // [count]{pc, frame_desc, map, ic}
};

/// Engine binding: the runtime state a J1-compiled method executes against.
/// The context is a value — the executable reads/patches its TLAB snapshot
/// natively, and run_baseline() re-syncs the heap afterwards.
struct J1Bindings {
    gc::Heap* heap = nullptr;
    J1Context context{};
    std::vector<TaggedValue> constants;   // materialized pool (bindings-owned)
    std::vector<void*> klass_addr_table;  // for BaselineJob
    std::vector<int32_t> field_offsets;   // for BaselineJob
    // Standalone-mode klass storage (T0-prepared modules borrow instead).
    KlassRegistry registry;
    std::vector<Klass*> owned_klasses;
    const void* double_klass = nullptr;   // heap.double_klass() for the job
    /// Global safepoint poll word (SAFEPOINT_POLL templates load through
    /// the context pointer; the M2 handshake manager arms it).
    uint32_t safepoint_word = 0;
};

/// A published (executable) baseline method.
struct BaselineExecutable {
    infra::WritableCodeMemory memory;
    J1EntryFn entry = nullptr;
    J1OsrFn osr_entry = nullptr;  // nullptr when the method has no loops
    uint32_t method_id = 0;
};

/// Builds the engine binding for `module` against `heap` (+ optional T0
/// interpreter for the token-call fallback) IN PLACE: J1Context carries raw
/// pointers into the bindings' own vectors, so J1Bindings must never be
/// moved after construction (the context would dangle). Reuses T0's runtime
/// tables when the module already ran; otherwise builds a minimal klass
/// registry from the module's class/field tokens.
support::Result<void> make_j1_bindings(gc::Heap& heap, ugb::UGBModule& module,
                                       vm::Interpreter* interpreter,
                                       J1Bindings& out);

/// W^X publication: RW copy + flip. The returned executable owns its code
/// memory; entry is callable immediately after. When `range` is given the
/// mapping is carved from the shared code-range reservation so J1/J2 code
/// and LDPT arenas stay within rel32 reach of one another
/// (docs/ldpt.md section 1).
support::Result<BaselineExecutable> publish_baseline(
    const BaselineCode& code, infra::CodeRange* range = nullptr);

/// Maps a J1ErrorId to the T0-worded diagnostic (the parity contract:
/// identical observable error surface). Exposed for drivers and tests.
const char* diagnose_j1_error(uint32_t id) noexcept;

/// Runs a published method end-to-end and re-syncs the heap's TLAB view.
/// Errors map to T0-worded diagnostics via the context's last_error id.
support::Result<TaggedValue> run_baseline(BaselineExecutable& ex,
                                          J1Bindings& bindings,
                                          std::span<const TaggedValue> args);

/// The baseline compiler. M1: single-stencil instantiation (superstencil
/// promotion is exercised by promote_superstencils + fusion tests).
class BaselineJit {
public:
    explicit BaselineJit(StencilTable& stencils) noexcept : stencils_(stencils) {}

    support::Result<BaselineCode> compile(const BaselineJob& job);

    const StencilTable& stencils() const noexcept { return stencils_; }

private:
    StencilTable& stencils_;
};

}  // namespace vortex::j1
