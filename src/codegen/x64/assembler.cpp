#include "vortex/codegen/x64/assembler.hpp"

namespace vortex::codegen::x64 {

// ---- low-level encoders ------------------------------------------------------

void Assembler::rex_w(Reg reg, Reg rm_or_index) {
    uint8_t rex = 0x48;  // W set
    if (needs_rex(rm_or_index)) rex |= 0x1;  // B
    if (needs_rex(reg)) rex |= 0x4;          // R
    out_.emit8(rex);
}

void Assembler::rex_raw(uint8_t bits) {
    if (bits != 0x40) out_.emit8(bits);  // bare REX is a no-op prefix
}

void Assembler::modrm(uint8_t mod, Reg reg, Reg rm) {
    out_.emit8(static_cast<uint8_t>((mod << 6) | (reg_id(reg) << 3) | reg_id(rm)));
}

void Assembler::imm32(int32_t v) { out_.emit32(static_cast<uint32_t>(v)); }

void Assembler::emit_mem_operand(Reg reg, const Mem& m) {
    // SIB is required when index != no-index marker or base is RSP/RBP-mod-0.
    const bool no_index = reg_id(m.index) == 4 && m.scale == 0;
    const bool base_needs_sib = reg_id(m.base) == 4;   // RSP base
    // RBP (5) and R13 (13) both encode mod-0 rm as RIP-relative on real x86-64;
    // both must force a displacement (the M1 stencil corpus relies on this for
    // rbp-based vreg addressing).
    const bool special_base =
        reg_id(m.base) == 5 || reg_id(m.base) == 13;

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

void Assembler::mov_reg_imm32sx(Reg dst, int32_t imm) {
    // REX.W C7 /0 id: mov r/m64, imm32 (sign-extended).
    rex_w(dst, dst);
    out_.emit8(0xC7);
    modrm(0x3, Reg::RAX, dst);  // /0 = MOV
    imm32(imm);
}

void Assembler::sse_op(const uint8_t* prefix, size_t prefix_len, bool rex_w_set,
                       uint8_t opcode, Xmm reg, Xmm rm) {
    for (size_t i = 0; i < prefix_len; ++i) out_.emit8(prefix[i]);
    uint8_t rex = static_cast<uint8_t>(rex_w_set ? 0x48 : 0x40);
    if (needs_rex_xmm(reg)) rex |= 0x4;
    if (needs_rex_xmm(rm)) rex |= 0x1;
    rex_raw(rex);
    out_.emit8(0x0F);
    out_.emit8(opcode);
    modrm(0x3, static_cast<Reg>(xmm_id(reg) & 0x7),
          static_cast<Reg>(xmm_id(rm) & 0x7));
}

void Assembler::movsd_xmm_mem(Xmm dst, const Mem& src) {
    const uint8_t pfx[1] = {0xF2};
    for (uint8_t b : pfx) out_.emit8(b);
    uint8_t rex = 0x40;
    if (needs_rex_xmm(dst)) rex |= 0x4;
    if (needs_rex(src.base)) rex |= 0x1;
    rex_raw(rex);
    out_.emit8(0x0F);
    out_.emit8(0x10);
    emit_mem_operand(static_cast<Reg>(xmm_id(dst) & 0x7), src);
}

void Assembler::movsd_mem_xmm(const Mem& dst, Xmm src) {
    out_.emit8(0xF2);
    uint8_t rex = 0x40;
    if (needs_rex_xmm(src)) rex |= 0x4;
    if (needs_rex(dst.base)) rex |= 0x1;
    rex_raw(rex);
    out_.emit8(0x0F);
    out_.emit8(0x11);
    emit_mem_operand(static_cast<Reg>(xmm_id(src) & 0x7), dst);
}

void Assembler::movsd_xmm_xmm(Xmm dst, Xmm src) {
    const uint8_t pfx[1] = {0xF2};
    sse_op(pfx, 1, false, 0x11, dst, src);  // movsd r/m, xmm (reg,reg form)
}

void Assembler::movq_xmm_gpr(Xmm dst, Reg src) {
    const uint8_t pfx[1] = {0x66};
    for (uint8_t b : pfx) out_.emit8(b);
    uint8_t rex = 0x48;  // W set: 64-bit GPR source
    if (needs_rex_xmm(dst)) rex |= 0x4;
    if (needs_rex(src)) rex |= 0x1;
    out_.emit8(rex);
    out_.emit8(0x0F);
    out_.emit8(0x6E);
    modrm(0x3, static_cast<Reg>(xmm_id(dst) & 0x7), src);
}

void Assembler::movq_gpr_xmm(Reg dst, Xmm src) {
    out_.emit8(0x66);
    uint8_t rex = 0x48;
    if (needs_rex(dst)) rex |= 0x4;
    if (needs_rex_xmm(src)) rex |= 0x1;
    out_.emit8(rex);
    out_.emit8(0x0F);
    out_.emit8(0x7E);
    modrm(0x3, dst, static_cast<Reg>(xmm_id(src) & 0x7));
}

void Assembler::pxor_xmm_xmm(Xmm dst, Xmm src) {
    const uint8_t pfx[1] = {0x66};
    sse_op(pfx, 1, false, 0xEF, dst, src);
}

void Assembler::addsd(Xmm dst, Xmm src) {
    const uint8_t pfx[1] = {0xF2};
    sse_op(pfx, 1, false, 0x58, dst, src);
}

void Assembler::subsd(Xmm dst, Xmm src) {
    const uint8_t pfx[1] = {0xF2};
    sse_op(pfx, 1, false, 0x5C, dst, src);
}

void Assembler::mulsd(Xmm dst, Xmm src) {
    const uint8_t pfx[1] = {0xF2};
    sse_op(pfx, 1, false, 0x59, dst, src);
}

void Assembler::divsd(Xmm dst, Xmm src) {
    const uint8_t pfx[1] = {0xF2};
    sse_op(pfx, 1, false, 0x5E, dst, src);
}

void Assembler::ucomisd(Xmm dst, Xmm src) {
    const uint8_t pfx[1] = {0x66};
    sse_op(pfx, 1, false, 0x2E, dst, src);
}

void Assembler::cvtsi2sd(Xmm dst, Reg src) {
    out_.emit8(0xF2);
    uint8_t rex = 0x48;  // W set: 64-bit GPR source
    if (needs_rex_xmm(dst)) rex |= 0x4;
    if (needs_rex(src)) rex |= 0x1;
    out_.emit8(rex);
    out_.emit8(0x0F);
    out_.emit8(0x2A);
    modrm(0x3, static_cast<Reg>(xmm_id(dst) & 0x7), src);
}

void Assembler::cvttsd2si(Reg dst, Xmm src) {
    out_.emit8(0xF2);
    uint8_t rex = 0x48;
    if (needs_rex(dst)) rex |= 0x4;
    if (needs_rex_xmm(src)) rex |= 0x1;
    out_.emit8(rex);
    out_.emit8(0x0F);
    out_.emit8(0x2C);
    modrm(0x3, dst, static_cast<Reg>(xmm_id(src) & 0x7));
}

void Assembler::mov_reg_reg(Reg dst, Reg src) {
    rex_w(src, dst);        // reg field = src, rm field = dst
    out_.emit8(0x89);
    modrm(0x3, src, dst);
}

uint8_t Assembler::mem_rex(bool rex_w_set, uint8_t reg_field,
                           const Mem& m) const {
    uint8_t rex = static_cast<uint8_t>(rex_w_set ? 0x48 : 0x40);
    if (reg_field >= 8) rex |= 0x4;
    if (needs_rex(m.base)) rex |= 0x1;
    return rex;
}

void Assembler::mov_reg32_mem(Reg dst, const Mem& src) {
    rex_raw(mem_rex(false, reg_id(dst), src));
    out_.emit8(0x8B);
    emit_mem_operand(dst, src);
}

void Assembler::mov_mem_imm32(const Mem& dst, int32_t imm) {
    // C7 /0 id: 32-bit store (64-bit store variant is mov_mem_imm32sx).
    rex_raw(mem_rex(false, 0, dst));
    out_.emit8(0xC7);
    emit_mem_operand(Reg::RAX, dst);  // /0 = MOV
    imm32(imm);
}

void Assembler::mov_mem_imm32sx(const Mem& dst, int32_t imm) {
    // REX.W C7 /0 id: 64-bit store of a sign-extended imm32.
    rex_raw(mem_rex(true, 0, dst));
    out_.emit8(0xC7);
    emit_mem_operand(Reg::RAX, dst);  // /0 = MOV
    imm32(imm);
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

void Assembler::cmp_mem_reg(const Mem& m, Reg src) {
    rex_w(src, m.base);
    out_.emit8(0x39);
    emit_mem_operand(src, m);
}

void Assembler::cmp_reg_mem(Reg dst, const Mem& m) {
    rex_w(dst, m.base);
    out_.emit8(0x3B);
    emit_mem_operand(dst, m);
}

void Assembler::call_mem(const Mem& target) {
    rex_raw(mem_rex(false, 2, target));  // FF /2 = CALL r/m64
    out_.emit8(0xFF);
    emit_mem_operand(Reg::RDX, target);  // /2 = CALL
}

void Assembler::test_reg_imm8(Reg dst, uint8_t imm) {
    rex_w(dst, dst);
    out_.emit8(0xF6);
    modrm(0x3, Reg::RAX, dst);  // /0 = TEST
    out_.emit8(imm);
}

void Assembler::test_reg_imm32(Reg dst, int32_t imm) {
    rex_w(dst, dst);
    out_.emit8(0xF7);
    modrm(0x3, Reg::RAX, dst);  // /0 = TEST
    imm32(imm);
}

void Assembler::or_reg_imm8(Reg dst, int8_t imm) {
    rex_w(dst, dst);
    out_.emit8(0x83);
    modrm(0x3, Reg::RCX, dst);  // /1 = OR
    out_.emit8(static_cast<uint8_t>(imm));
}

void Assembler::and_reg_imm8(Reg dst, int8_t imm) {
    rex_w(dst, dst);
    out_.emit8(0x83);
    modrm(0x3, Reg::RSP, dst);  // /4 = AND
    out_.emit8(static_cast<uint8_t>(imm));
}

void Assembler::shl_reg_cl(Reg dst) {
    rex_w(dst, dst);
    out_.emit8(0xD3);
    modrm(0x3, Reg::RAX, dst);  // /4 = SHL
}

void Assembler::shr_reg_cl(Reg dst) {
    rex_w(dst, dst);
    out_.emit8(0xD3);
    modrm(0x3, Reg::RCX, dst);  // /5 = SHR
}

void Assembler::sar_reg_cl(Reg dst) {
    rex_w(dst, dst);
    out_.emit8(0xD3);
    modrm(0x3, Reg::RBP, dst);  // /7 = SAR
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
