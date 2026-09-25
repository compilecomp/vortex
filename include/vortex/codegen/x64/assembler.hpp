// x86-64 machine-code assembler (System V ABI target).
//
// This is the real encoder the J1 stencil corpus and the tier backends build
// on: ModRM/SIB encoding, immediate forms, rip-relative-free absolute forms,
// and relocations for branches/calls. Golden-byte tests pin the encoding
// (tests/test_codegen_infra.cpp).
#pragma once

#include <cstdint>
#include <optional>

#include "vortex/codegen/codegen.hpp"

namespace vortex::codegen::x64 {

// ---- registers -----------------------------------------------------------------

enum class Reg : uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3,
    RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

constexpr uint8_t reg_id(Reg r) noexcept { return static_cast<uint8_t>(r); }
constexpr bool needs_rex(Reg r) noexcept { return reg_id(r) >= 8; }

/// XMM vector/scalar registers (SSE2 scalar-double ops used by the F64
/// stencils; see tests/test_codegen_infra.cpp golden pins).
enum class Xmm : uint8_t {
    XMM0 = 0, XMM1 = 1, XMM2 = 2, XMM3 = 3,
    XMM4 = 4, XMM5 = 5, XMM6 = 6, XMM7 = 7,
    XMM8 = 8, XMM9 = 9, XMM10 = 10, XMM11 = 11,
    XMM12 = 12, XMM13 = 13, XMM14 = 14, XMM15 = 15,
};

constexpr uint8_t xmm_id(Xmm r) noexcept { return static_cast<uint8_t>(r); }
constexpr bool needs_rex_xmm(Xmm r) noexcept { return xmm_id(r) >= 8; }

/// Memory operand: [base + scale*index + displacement].
struct Mem {
    Reg base = Reg::RBP;
    Reg index = Reg::RSP;  // RSP encodes "no index" (SIB)
    uint8_t scale = 0;     // 0,1,2,3 => *1,*2,*4,*8
    int32_t disp = 0;
};

// ---- assembler -----------------------------------------------------------------

class Assembler {
public:
    explicit Assembler(CodeBuffer& out) noexcept : out_(out) {}

    // ---- moves ---------------------------------------------------------------
    void mov_reg_imm64(Reg dst, uint64_t imm);          // REX.W B8+rd io
    void mov_reg_imm32sx(Reg dst, int32_t imm);         // REX.W C7 /0 id (sign-ext)
    void mov_reg_reg(Reg dst, Reg src);                 // REX.W 89 /r
    void mov_mem_reg(const Mem& dst, Reg src);          // REX.W 89 /r
    void mov_reg_mem(Reg dst, const Mem& src);          // REX.W 8B /r
    void mov_reg32_mem(Reg dst, const Mem& src);        // 8B /r   (32-bit load)
    void mov_mem_imm32(const Mem& dst, int32_t imm);    // C7 /0 id (32-bit store)
    void mov_mem_imm32sx(const Mem& dst, int32_t imm);  // REX.W C7 /0 (64-bit, sign-ext)
    void mov_mem_imm8(const Mem& dst, int8_t imm);      // C6 /0 ib (8-bit store:
                                                        // card-table barriers)

    // ---- scalar double (SSE2) -------------------------------------------------
    void movsd_xmm_mem(Xmm dst, const Mem& src);        // F2 0F 10 /r
    void movsd_mem_xmm(const Mem& dst, Xmm src);        // F2 0F 11 /r
    void movsd_xmm_xmm(Xmm dst, Xmm src);               // F2 0F 11 /r (reg,reg)
    void movq_xmm_gpr(Xmm dst, Reg src);                // 66 REX.W 0F 6E /r
    void movq_gpr_xmm(Reg dst, Xmm src);                // 66 REX.W 0F 7E /r
    void pxor_xmm_xmm(Xmm dst, Xmm src);                // 66 0F EF /r
    void addsd(Xmm dst, Xmm src);                       // F2 0F 58 /r
    void subsd(Xmm dst, Xmm src);                       // F2 0F 5C /r
    void mulsd(Xmm dst, Xmm src);                       // F2 0F 59 /r
    void divsd(Xmm dst, Xmm src);                       // F2 0F 5E /r
    void ucomisd(Xmm dst, Xmm src);                     // 66 0F 2E /r
    void cvtsi2sd(Xmm dst, Reg src);                    // F2 REX.W 0F 2A /r
    void cvttsd2si(Reg dst, Xmm src);                   // F2 REX.W 0F 2C /r

    // ---- arithmetic (64-bit) ---------------------------------------------------
    void add_reg_reg(Reg dst, Reg src);                 // REX.W 01 /r
    void or_reg_reg(Reg dst, Reg src);                  // REX.W 09 /r
    void and_reg_reg(Reg dst, Reg src);                 // REX.W 21 /r
    void add_reg_imm32(Reg dst, int32_t imm);           // REX.W 81 /0 id
    void sub_reg_reg(Reg dst, Reg src);                 // REX.W 29 /r
    void sub_reg_imm32(Reg dst, int32_t imm);
    void and_reg_imm32(Reg dst, int32_t imm);           // REX.W 81 /4 id
    void or_reg_imm32(Reg dst, int32_t imm);            // REX.W 81 /1 id
    void xor_reg_reg(Reg dst, Reg src);                 // REX.W 31 /r
    void cmp_reg_imm32(Reg dst, int32_t imm);           // REX.W 81 /7 id
    void cmp_reg_reg(Reg dst, Reg src);                 // REX.W 39 /r
    void cmp_mem_reg(const Mem& m, Reg src);            // REX.W 39 /r (r/m, reg)
    void cmp_reg_mem(Reg dst, const Mem& m);            // REX.W 3B /r (reg, r/m)
    void call_mem(const Mem& target);                   // FF /2
    void test_reg_imm8(Reg dst, uint8_t imm);           // REX.W F6 /0 ib
    void test_reg_imm32(Reg dst, int32_t imm);          // REX.W F7 /0 id
    void or_reg_imm8(Reg dst, int8_t imm);              // REX.W 83 /1 ib
    void and_reg_imm8(Reg dst, int8_t imm);             // REX.W 83 /4 ib
    void shl_reg_cl(Reg dst);                           // REX.W D3 /4
    void shr_reg_cl(Reg dst);                           // REX.W D3 /5
    void sar_reg_cl(Reg dst);                           // REX.W D3 /7
    void imul_reg_reg(Reg dst, Reg src);                // REX.W 0F AF /r
    void inc_reg(Reg r);                                // REX.W FF /0
    void dec_reg(Reg r);                                // REX.W FF /1
    void neg_reg(Reg r);                                // REX.W F7 /3

    // ---- shifts ------------------------------------------------------------------
    void shift_reg_imm8(Reg dst, uint8_t count, uint8_t op_ext);
    void shl_reg_imm8(Reg dst, uint8_t count);          // REX.W C1 /4 ib
    void shr_reg_imm8(Reg dst, uint8_t count);          // REX.W C1 /5 ib
    void sar_reg_imm8(Reg dst, uint8_t count);          // REX.W C1 /7 ib

    // ---- control flow -------------------------------------------------------------
    void jmp_rel32(int32_t rel);                        // E9 cd
    void jmp_reg(Reg target);                           // FF /4
    void jcc_rel32(uint8_t cc, int32_t rel);            // 0F 80+cc cd
    void call_rel32(int32_t rel);                       // E8 cd
    void call_reg(Reg target);                          // FF /2
    void ret();                                         // C3
    void push_reg(Reg r);                               // 50+rd
    void pop_reg(Reg r);                                // 58+rd
    void lea_reg_mem(Reg dst, const Mem& src);          // REX.W 8D /r
    /// LEA with a scaled index and no base: dst = index*scale + disp
    /// (mod=00 rm=101 + SIB). Flag preserving — usable between a compare
    /// and its JCC.
    void lea_reg_scaled_disp(Reg dst, Reg index, uint8_t scale_log2,
                             int32_t disp);
    /// SETcc on the low byte of a register (0F 90+cc /r, mod=11). Flag
    /// preserving: the following JCC still sees the compare's flags. Only
    /// codes 0-3 (al/cl/dl/bl) are accepted without extension bytes.
    void setcc(uint8_t cc, Reg r8);

    // ---- padding -------------------------------------------------------------------
    void nop(size_t bytes);                             // multi-byte NOPs

    // ---- branch fixation ------------------------------------------------------------
    /// Records a 32-bit branch placeholder at the current position and returns
    /// its byte offset; `bind_placeholder(off)` patches rel32 = target - (off+4).
    size_t placeholder_jmp();
    size_t placeholder_jcc(uint8_t cc);
    size_t placeholder_call();
    void bind_placeholder(size_t at);

    size_t current_offset() const noexcept { return out_.size(); }

private:
    void rex_w(Reg reg, Reg rm_or_index);               // emit REX prefix byte
    /// REX with explicit bits: W=bit3, R=bit2, X=bit1, B=bit0.
    void rex_raw(uint8_t bits);
    void modrm(uint8_t mod, Reg reg, Reg rm);
    /// Extension-field variant for group opcodes (shifts, 81 /N families).
    void modrm_ext(uint8_t mod, uint8_t ext, Reg rm);
    void emit_mem_operand(Reg reg, const Mem& m);       // modrm + sib + disp
    /// SSE2 operand encoding: prefix bytes, optional REX (R from `reg_is_xmm_high`,
    /// B from `rm_is_xmm_high`), opcode, modrm. GPR register fields reuse reg/reg.
    void sse_op(const uint8_t* prefix, size_t prefix_len, bool rex_w_set,
                uint8_t opcode, Xmm reg, Xmm rm);
    /// REX byte for a memory-form operation: R bit from `reg`, B from mem base.
    uint8_t mem_rex(bool rex_w_set, uint8_t reg_field, const Mem& m) const;
    void imm32(int32_t v);

    CodeBuffer& out_;
};

// Condition codes (x86 CC field of 0F 80+cc).
enum CC : uint8_t {
    CC_O = 0x0, CC_NO = 0x1, CC_B = 0x2, CC_AE = 0x3,
    CC_E = 0x4, CC_NE = 0x5, CC_BE = 0x6, CC_A = 0x7,
    CC_S = 0x8, CC_NS = 0x9, CC_P = 0xA, CC_NP = 0xB,
    CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF,
};

}  // namespace vortex::codegen::x64
