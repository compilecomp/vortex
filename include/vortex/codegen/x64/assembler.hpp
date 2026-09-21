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
    void mov_reg_reg(Reg dst, Reg src);                 // REX.W 89 /r
    void mov_mem_reg(const Mem& dst, Reg src);          // REX.W 89 /r
    void mov_reg_mem(Reg dst, const Mem& src);          // REX.W 8B /r

    // ---- arithmetic (64-bit) ---------------------------------------------------
    void add_reg_reg(Reg dst, Reg src);                 // REX.W 01 /r
    void add_reg_imm32(Reg dst, int32_t imm);           // REX.W 81 /0 id
    void sub_reg_reg(Reg dst, Reg src);                 // REX.W 29 /r
    void sub_reg_imm32(Reg dst, int32_t imm);
    void and_reg_imm32(Reg dst, int32_t imm);           // REX.W 81 /4 id
    void or_reg_imm32(Reg dst, int32_t imm);            // REX.W 81 /1 id
    void xor_reg_reg(Reg dst, Reg src);                 // REX.W 31 /r
    void cmp_reg_imm32(Reg dst, int32_t imm);           // REX.W 81 /7 id
    void cmp_reg_reg(Reg dst, Reg src);                 // REX.W 39 /r
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
    void modrm(uint8_t mod, Reg reg, Reg rm);
    void emit_mem_operand(Reg reg, const Mem& m);       // modrm + sib + disp
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
