#include "vortex/codegen/x64/assembler.hpp"

namespace vortex::codegen::x64 {

// ---- low-level encoders ------------------------------------------------------

void Assembler::rex_w(Reg reg, Reg rm_or_index) {
    uint8_t rex = 0x48;  // W set
    if (needs_rex(rm_or_index)) rex |= 0x1;  // B
    if (needs_rex(reg)) rex |= 0x4;          // R
    out_.emit8(rex);
}

void Assembler::modrm(uint8_t mod, Reg reg, Reg rm) {
    out_.emit8(static_cast<uint8_t>((mod << 6) | (reg_id(reg) << 3) | reg_id(rm)));
}

void Assembler::imm32(int32_t v) { out_.emit32(static_cast<uint32_t>(v)); }

void Assembler::emit_mem_operand(Reg reg, const Mem& m) {
    // SIB is required when index != no-index marker or base is RSP/RBP-mod-0.
    const bool no_index = reg_id(m.index) == 4 && m.scale == 0;
    const bool base_needs_sib = reg_id(m.base) == 4;   // RSP base
    const bool special_base = reg_id(m.base) == 5;     // RBP base: mod 0 needs disp

    uint8_t mod;
    if (m.disp == 0 && !special_base) {
        mod = 0x0;
    } else if (m.disp >= -128 && m.disp <= 127) {
        mod = 0x1;
    } else {
        mod = 0x2;
    }

    if (no_index && !base_needs_sib) {
        modrm(mod, reg, m.base);
        if (mod == 0x1) {
            out_.emit8(static_cast<uint8_t>(static_cast<int8_t>(m.disp)));
        } else if (mod == 0x2) {
            imm32(m.disp);
        }
        return;
    }

    // SIB byte: scale | index | base
    const uint8_t scale_bits = static_cast<uint8_t>(m.scale & 0x3);
    modrm(mod, reg, Reg::RSP);  // rm = 100 escapes to SIB
    out_.emit8(static_cast<uint8_t>((scale_bits << 6) | (reg_id(m.index) << 3) |
                                    reg_id(m.base)));
    if (mod == 0x0 && special_base) {
        out_.emit8(0);  // RBP/R13 base with no displacement still needs disp8=0
    } else if (mod == 0x1) {
        out_.emit8(static_cast<uint8_t>(static_cast<int8_t>(m.disp)));
    } else if (mod == 0x2) {
        imm32(m.disp);
    }
}

// ---- moves --------------------------------------------------------------------

void Assembler::mov_reg_imm64(Reg dst, uint64_t imm) {
    // REX.W + B8+r: 64-bit immediate moves to register (no ModRM).
    uint8_t rex = 0x48;
    if (needs_rex(dst)) rex |= 0x1;
    out_.emit8(rex);
    out_.emit8(static_cast<uint8_t>(0xB8 + (reg_id(dst) & 0x7)));
    out_.emit64(imm);
}

void Assembler::mov_reg_reg(Reg dst, Reg src) {
    rex_w(src, dst);        // reg field = src, rm field = dst
    out_.emit8(0x89);
    modrm(0x3, src, dst);
}

void Assembler::mov_mem_reg(const Mem& dst, Reg src) {
    rex_w(src, dst.base);
    out_.emit8(0x89);
    emit_mem_operand(src, dst);
}

void Assembler::mov_reg_mem(Reg dst, const Mem& src) {
    rex_w(dst, src.base);
    out_.emit8(0x8B);
    emit_mem_operand(dst, src);
}

void Assembler::lea_reg_mem(Reg dst, const Mem& src) {
    rex_w(dst, src.base);
    out_.emit8(0x8D);
    emit_mem_operand(dst, src);
}

// ---- arithmetic -----------------------------------------------------------------

void Assembler::add_reg_reg(Reg dst, Reg src) {
    rex_w(src, dst);
    out_.emit8(0x01);
    modrm(0x3, src, dst);
}

void Assembler::add_reg_imm32(Reg dst, int32_t v) {
    rex_w(dst, dst);
    out_.emit8(0x81);
    modrm(0x3, Reg::RAX, dst);  // /0 = ADD
    imm32(v);
}

void Assembler::sub_reg_reg(Reg dst, Reg src) {
    rex_w(src, dst);
    out_.emit8(0x29);
    modrm(0x3, src, dst);
}

void Assembler::sub_reg_imm32(Reg dst, int32_t v) {
    rex_w(dst, dst);
    out_.emit8(0x81);
    modrm(0x3, Reg::RBP, dst);  // /5 = SUB
    imm32(v);
}

void Assembler::and_reg_imm32(Reg dst, int32_t v) {
    rex_w(dst, dst);
    out_.emit8(0x81);
    modrm(0x3, Reg::RSP, dst);  // /4 = AND
    imm32(v);
}

void Assembler::or_reg_imm32(Reg dst, int32_t v) {
    rex_w(dst, dst);
    out_.emit8(0x81);
    modrm(0x3, Reg::RCX, dst);  // /1 = OR
    imm32(v);
}

void Assembler::xor_reg_reg(Reg dst, Reg src) {
    rex_w(src, dst);
    out_.emit8(0x31);
    modrm(0x3, src, dst);
}

void Assembler::cmp_reg_imm32(Reg dst, int32_t v) {
    rex_w(dst, dst);
    out_.emit8(0x81);
    modrm(0x3, Reg::RDI, dst);  // /7 = CMP
    imm32(v);
}

void Assembler::cmp_reg_reg(Reg dst, Reg src) {
    rex_w(src, dst);
    out_.emit8(0x39);
    modrm(0x3, src, dst);
}

void Assembler::imul_reg_reg(Reg dst, Reg src) {
    rex_w(dst, src);
    out_.emit8(0x0F);
    out_.emit8(0xAF);
    modrm(0x3, dst, src);
}

void Assembler::inc_reg(Reg r) {
    rex_w(r, r);
    out_.emit8(0xFF);
    modrm(0x3, Reg::RAX, r);  // /0 = INC
}

void Assembler::dec_reg(Reg r) {
    rex_w(r, r);
    out_.emit8(0xFF);
    modrm(0x3, Reg::RCX, r);  // /1 = DEC
}

void Assembler::neg_reg(Reg r) {
    rex_w(r, r);
    out_.emit8(0xF7);
    modrm(0x3, Reg::RBX, r);  // /3 = NEG
}

// ---- shifts ---------------------------------------------------------------------

void Assembler::shift_reg_imm8(Reg dst, uint8_t count, uint8_t op_ext) {
    rex_w(dst, dst);
    out_.emit8(0xC1);
    modrm(0x3, static_cast<Reg>(op_ext), dst);
    out_.emit8(count);
}

void Assembler::shl_reg_imm8(Reg dst, uint8_t count) { shift_reg_imm8(dst, count, 4); }
void Assembler::shr_reg_imm8(Reg dst, uint8_t count) { shift_reg_imm8(dst, count, 5); }
void Assembler::sar_reg_imm8(Reg dst, uint8_t count) { shift_reg_imm8(dst, count, 7); }

// ---- control flow ------------------------------------------------------------------

void Assembler::jmp_rel32(int32_t rel) {
    out_.emit8(0xE9);
    imm32(rel);
}

void Assembler::jmp_reg(Reg target) {
    out_.emit8(0xFF);
    modrm(0x3, Reg::RSP, target);  // /4 = JMP
}

void Assembler::jcc_rel32(uint8_t cc, int32_t rel) {
    out_.emit8(0x0F);
    out_.emit8(static_cast<uint8_t>(0x80 + cc));
    imm32(rel);
}

void Assembler::call_rel32(int32_t rel) {
    out_.emit8(0xE8);
    imm32(rel);
}

void Assembler::call_reg(Reg target) {
    out_.emit8(0xFF);
    modrm(0x3, Reg::RDX, target);  // /2 = CALL
}

void Assembler::ret() { out_.emit8(0xC3); }

void Assembler::push_reg(Reg r) {
    if (needs_rex(r)) out_.emit8(0x41);
    out_.emit8(static_cast<uint8_t>(0x50 + (reg_id(r) & 0x7)));
}

void Assembler::pop_reg(Reg r) {
    if (needs_rex(r)) out_.emit8(0x41);
    out_.emit8(static_cast<uint8_t>(0x58 + (reg_id(r) & 0x7)));
}

// ---- padding --------------------------------------------------------------------

void Assembler::nop(size_t bytes) {
    // Multi-byte canonical NOPs (padding for alignment, patch sleds).
    static const uint8_t kNop9[9] = {0x66, 0x0F, 0x1F, 0x84, 0x00,
                                     0x00, 0x00, 0x00, 0x00};
    static const uint8_t kNop4[4] = {0x0F, 0x1F, 0x40, 0x00};
    static const uint8_t kNop1[1] = {0x90};
    while (bytes >= 9) {
        out_.emit_bytes(kNop9, 9);
        bytes -= 9;
    }
    if (bytes >= 4) {
        out_.emit_bytes(kNop4, 4);
        bytes -= 4;
    }
    while (bytes > 0) {
        out_.emit_bytes(kNop1, 1);
        --bytes;
    }
}

// ---- branch placeholders -----------------------------------------------------------

size_t Assembler::placeholder_jmp() {
    const size_t at = out_.size();
    out_.emit8(0xE9);
    imm32(0);
    return at + 1;  // relocation points at the immediate
}

size_t Assembler::placeholder_jcc(uint8_t cc) {
    const size_t at = out_.size();
    out_.emit8(0x0F);
    out_.emit8(static_cast<uint8_t>(0x80 + cc));
    imm32(0);
    return at + 2;
}

size_t Assembler::placeholder_call() {
    const size_t at = out_.size();
    out_.emit8(0xE8);
    imm32(0);
    return at + 1;
}

void Assembler::bind_placeholder(size_t at) {
    // rel32 = target - (imm_end). The stored offset `at` points at the
    // immediate; imm_end = at + 4.
    const int32_t rel =
        static_cast<int32_t>(out_.size() - (at + 4));
    uint8_t* p = out_.code().data() + at;
    p[0] = static_cast<uint8_t>(rel & 0xFF);
    p[1] = static_cast<uint8_t>((rel >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((rel >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((rel >> 24) & 0xFF);
}

}  // namespace vortex::codegen::x64
