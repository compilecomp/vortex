// J1 x86-64 stencil corpus (docs/tier-j1.md sections 2-3, docs/roadmap.md M1).
//
// The corpus holds precompiled machine-code templates for the core opcode
// set. Each template is assembled once (low compile latency = memcpy + patch,
// never re-encode) and carries patch sites that the baseline compiler rewrites
// per instruction instantiation.
//
// Patch site operand semantics (PatchSite::operand values):
//
//   PatchKind::VirtualRegister  — the site's 4 bytes are a disp32 in
//       [rbp + disp] addressing; operand selects the instruction operand
//       whose vreg displacement is written:
//         0 = dst   1 = src0   2 = src1   3 = src2
//         4 = PA_ArgBase   (lea &vreg[src0]: call argument window)
//         5 = PA_VregBase  (lea &vreg[0]: whole frame, deopt/OSR)
//   PatchKind::ConstantIndex    — the site's bytes are immediates:
//         0 = PA_ConstSmi     (8 bytes, imm64 = (int64)(int32)meta << 1)
//         1 = PA_ConstPoolOff (4 bytes, imm32 = 8 * meta)
//         2 = PA_KlassOff     (4 bytes, imm32 = 8 * klass_token)
//         3 = PA_KlassAddr    (8 bytes, imm64 = double-box klass pointer)
//         4 = PA_FieldByteOff (4 bytes, imm32 = 16 + 8 * ic_slot target)
//         5 = PA_FieldCount   (4 bytes, imm32 = klass field count)
//         6 = PA_Pc           (4 bytes, imm32 = bytecode pc of the site)
//         7 = PA_RegCount     (4 bytes, imm32 = method register_count)
//   PatchKind::BranchTarget     — the site's 4 bytes are a rel32 immediate:
//         0 = jump to the native offset of the instruction's branch target
//         1 = jump to the method error epilogue (rax = error id)
//         2 = jump to the method ok epilogue
//   PatchKind::CallTarget       — 0 = rel32 call to the callee's J1 entry
//   PatchKind::IcSlot           — inline-cache guard patch points:
//         0 = guard compare immediate (8 bytes: expected klass pointer)
//         1 = rel32 to this instruction's slow-path block; the two bytes
//             before it flip the template's always-slow `jmp rel32`
//             (0x90 0xE9 ...) into a guarded `jne rel32` (0F 85) once the
//             compare immediates are in place (docs/tier-j1.md section 7).
//
// A stencil is "speculative" when it implements a typed opcode with a
// canonical fallback inside the template itself (failure path calls the
// canonical-binop helper, mirroring T0's in-loop fallback).
#pragma once

#include <cstdint>
#include <unordered_map>

#include "vortex/j1/stencil.hpp"

namespace vortex::j1 {

/// Builds the default x86-64 corpus: typed + generic arithmetic, constants,
/// moves, comparisons, conversions, control flow, calls, allocation, field
/// and array access, guards, safepoint poll. Returns the number of stencils
/// registered (superstencils are promoted separately — see
/// promote_superstencils).
size_t build_default_corpus(StencilTable& table);

/// Fuses adjacent stencil pairs into superstencils (docs/tier-j1.md section
/// 3). Because J1 stencils communicate exclusively through frame memory,
/// concatenation of any adjacent pair is semantically safe; fusion wins by
/// removing one instantiation + patch round per fused instruction.
///
/// `bigram_counts` maps (first_op << 16 | second_op) -> executions, exactly
/// the table T0's record_bigram feeds. Pairs whose count reached the
/// threshold and whose members both exist in the corpus are fused.
struct SuperstencilPromotionStats {
    uint32_t candidates = 0;
    uint32_t promoted = 0;
};
SuperstencilPromotionStats promote_superstencils(
    StencilTable& table, const std::unordered_map<uint64_t, uint64_t>& bigram_counts,
    uint32_t threshold);

/// Re-patches an inline-cache guard inside an instantiated (still-RW) code
/// buffer: writes `klass` into the compare immediate and flips the always-
/// slow jump into a conditional branch to `slow_rel_target`. `sites` must be
/// the two IcSlot sites of the stencil instance (guard imm + rel32). This is
/// the guard-strengthening entry point used at compile time (and, once the
/// M6 W^X repatcher lands, at runtime).
void patch_ic_guard(uint8_t* code, const PatchSite& guard_imm,
                    const PatchSite& slow_rel, uint64_t klass);

/// Representative vreg displacement used inside templates (any value in the
/// disp32 range works; -160 keeps the mod=10 encoding uniform).
constexpr int32_t kTemplateVregDisp = -160;

/// Size in bytes of the reserved guard patch-point sled.
constexpr uint32_t kGuardSledBytes = 16;

}  // namespace vortex::j1
