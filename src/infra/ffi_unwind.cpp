// CEP:FILE: src/infra/ffi_unwind.cpp
// CEP:WHAT: Infra 5 unified unwinding implementation (M6-c,
//           docs/infrastructure/05-ffi.md "Unified unwinder"): one RBP-chain
//           walker that names every frame of a mixed stack — JIT frames
//           through the M5 published-code registry (GcCodeRegistry),
//           trampoline frames through the FfiTrampolineRegistry, native
//           frames through dladdr — plus walk_current's builtin-capture
//           entry point. The exception bridge itself is the header's
//           ffi_boundary_call template (try/catch at the boundary, Rule 132;
//           foreign exceptions never unwind through guest frames).
// CEP:WHY: Diagnostics, the GC's stack scan, and the deopt machinery all
//           need ONE frame-naming protocol over stacks that mix compiled
//           guest code, FFI stubs, and native code; the walker is the
//           contract that keeps those consumers from each re-implementing
//           the chain rules.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: The walk terminates on a null pc, a null RBP, a
//           non-monotonic RBP move, a step beyond the 8 MiB stack-adjacency
//           bound, or max_frames. The plausibility bounds make following a
//           corrupt chain UNLIKELY, not impossible (a garbage slot that
//           lands within one plausible step is still dereferenced; a corrupt
//           chain is already undefined behavior) — this is a diagnostics-
//           path API, not a guest hot path. Never raises.
// CEP:ASSUMES: Every executable frame keeps a real RBP chain — JIT frames
//           and trampolines chain by construction; native C++ frames keep
//           frame pointers because the build sets -fno-omit-frame-pointer
//           in every mode (docs/infrastructure/05-ffi.md DoD 8). Native
//           frames that violate the chain (hand-written asm, third-party
//           libs) truncate the walk at that frame — documented limitation,
//           not a misclassification. Single mutator (M0 contract): the
//           registries are not locked.
// CEP:COST: @cold diagnostics path. Per frame: two registry scans (both
//           linear over handfuls of live entries) + one dladdr on native
//           frames.
// CEP:EVIDENCE: tests/test_ffi.cpp — mixed-walk shape through a real J1
//           published frame and a trampoline, trampoline pc-offset pins,
//           walk_current capture, exception-bridge named failures.
#include "vortex/infra/ffi.hpp"

#include "vortex/gc/collector.hpp"

#include <algorithm>
#include <dlfcn.h>

namespace vortex::infra {
namespace {
/// Plausibility bound for one unwind step (see the walk's adjacency check).
constexpr uintptr_t kMaxFrameStep = 8u << 20;  // 8 MiB
}  // namespace

// CEP:WHAT: Mixed-stack RBP-chain walk with three-way frame classification.
// CEP:WHY: docs/infrastructure/05-ffi.md "Unified unwinder" — the single
//           frame-naming protocol for JIT/trampoline/native stacks.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Termination on null pc/RBP, non-monotonic chain move, or
//           max_frames (see module CEP:FAILURE). No allocation failures are
//           caught here — the vector grows or raises per std::vector (the
//           walker is a diagnostics-path API, not a guest hot path).
// CEP:ASSUMES: Frame pointers on every executable frame (module
//           CEP:ASSUMES); the initial (rbp, ret_pc) pair follows the
//           documented convention: rbp is the frame pointer of the frame
//           that contains ret_pc in its code.
// CEP:COST: @cold. Per frame: two linear registry scans + dladdr on native
//           frames.
// CEP:EVIDENCE: tests/test_ffi.cpp (the mixed-walk DoD 6 pins).
std::vector<UnifiedFrame> UnifiedUnwinder::walk(uintptr_t rbp,
                                                uintptr_t ret_pc,
                                                size_t max_frames) {
    std::vector<UnifiedFrame> out;
    uintptr_t pc = ret_pc;
    while (pc != 0 && out.size() < max_frames) {
        UnifiedFrame f;
        f.ip = pc;
        f.rbp = rbp;
        if (const gc::GcCodeRecord* jit = gc::gc_code_registry().lookup(pc)) {
            f.kind = UnifiedFrame::Kind::Jit;
            f.pc_offset =
                static_cast<uint32_t>(pc -
                                      reinterpret_cast<uintptr_t>(
                                          jit->code_base));
        } else if (const FfiTrampolineRegistry::Range* tr =
                       ffi_trampoline_registry().lookup(pc)) {
            f.kind = UnifiedFrame::Kind::Trampoline;
            f.pc_offset = static_cast<uint32_t>(pc - tr->lo);
        } else {
            f.kind = UnifiedFrame::Kind::Native;
            Dl_info info{};
            if (dladdr(reinterpret_cast<const void*>(pc), &info) != 0) {
                // Prefer the exact symbol; the module path is the fallback.
                // Both pointers live in the loader's link maps (stable for
                // the process lifetime).
                f.origin = info.dli_sname != nullptr ? info.dli_sname
                                                     : info.dli_fname;
            }
        }
        out.push_back(f);

        if (rbp == 0) break;
        const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(rbp);
        const uintptr_t next_rbp = frame[0];
        const uintptr_t next_pc = frame[1];
        // Non-monotonic OR non-adjacent: stop, never follow. A legitimate
        // unwind step moves RBP up by one frame (bytes to a few KiB); a
        // frameless function's saved-rbp slot holds arbitrary data that can
        // look monotonic — the stack-adjacency bound keeps that class from
        // ever being dereferenced (docs/infrastructure/05-ffi.md walk
        // termination list).
        if (next_rbp <= rbp || next_rbp - rbp > kMaxFrameStep) break;
        rbp = next_rbp;
        pc = next_pc;
    }
    return out;
}

// CEP:WHAT: Capture entry: describes the immediate CALLER as frame 0.
// CEP:WHY: Callers sample their own mixed stack without hand-capturing
//           register pairs; noinline + frame pointers keep the capture
//           honest (an inlined capture would describe the wrong frame).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Delegates to walk() (same termination contract).
// CEP:ASSUMES: Built with frame pointers (-fno-omit-frame-pointer, every
//           mode); the builtin pair describes the caller frame per the GCC
//           contract (return address 0 = into the caller, frame address 1 =
//           the caller's frame pointer). The nonzero-argument builtin form
//           is intentional and suppressed for this function only.
// CEP:COST: @cold. Two builtins + the walk itself.
// CEP:EVIDENCE: tests/test_ffi.cpp — walk_current capture pins (the first
//           frame names the capture site's caller).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
__attribute__((noinline)) std::vector<UnifiedFrame>
UnifiedUnwinder::walk_current(size_t max_frames) {
    return walk(reinterpret_cast<uintptr_t>(__builtin_frame_address(1)),
                reinterpret_cast<uintptr_t>(__builtin_return_address(0)),
                max_frames);
}
#pragma GCC diagnostic pop

}  // namespace vortex::infra
