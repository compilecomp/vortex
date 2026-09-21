// x64 assembler golden-byte tests + W^X execution + code cache + CPU dispatch.
#include "vortex_test.hpp"

#include "vortex/codegen/x64/assembler.hpp"
#include "vortex/infra/code_cache.hpp"
#include "vortex/infra/cpu_dispatch.hpp"
#include "vortex/infra/security.hpp"

using namespace vortex;
using namespace vortex::codegen;

VORTEX_TEST(x64_mov_reg_imm64_golden) {
    CodeBuffer buf;
    x64::Assembler a(buf);
    a.mov_reg_imm64(x64::Reg::RAX, 42);
    a.mov_reg_imm64(x64::Reg::R11, 0xDEADBEEFCAFEBABEull);
    // mov rax, 42  -> 48 B8 2A 00 00 00 00 00 00 00 (B8 form, imm64)
    // mov r11, imm64 -> 49 BB + imm64
    const std::vector<uint8_t>& c = buf.code();
    VORTEX_EXPECT_EQ(c.size(), size_t{10 + 10});
    VORTEX_EXPECT(c[0] == 0x48 && c[1] == 0xB8);           // mov rax, imm64
    VORTEX_EXPECT_EQ(c[2], uint8_t{42});
    VORTEX_EXPECT(c[10] == 0x49 && c[11] == 0xBB);         // mov r11, imm64
}

VORTEX_TEST(x64_mov_reg_reg_golden) {
    CodeBuffer buf;
    x64::Assembler a(buf);
    a.mov_reg_reg(x64::Reg::RAX, x64::Reg::RBX);  // 48 89 D8
    a.mov_reg_reg(x64::Reg::R9, x64::Reg::R15);   // 4D 89 F9
    const std::vector<uint8_t>& c = buf.code();
    VORTEX_EXPECT_EQ(c.size(), size_t{6});
    VORTEX_EXPECT(c[0] == 0x48 && c[1] == 0x89 && c[2] == 0xD8);
    VORTEX_EXPECT(c[3] == 0x4D && c[4] == 0x89 && c[5] == 0xF9);
}

VORTEX_TEST(x64_add_and_ret_golden) {
    CodeBuffer buf;
    x64::Assembler a(buf);
    a.add_reg_reg(x64::Reg::RAX, x64::Reg::RBX);  // 48 01 D8
    a.add_reg_imm32(x64::Reg::RAX, 1);            // 48 81 C0 01 00 00 00
    a.ret();                                      // C3
    const std::vector<uint8_t>& c = buf.code();
    VORTEX_EXPECT_EQ(c.size(), size_t{3 + 7 + 1});
    VORTEX_EXPECT(c[0] == 0x48 && c[1] == 0x01 && c[2] == 0xD8);
    VORTEX_EXPECT(c[3] == 0x48 && c[4] == 0x81 && c[5] == 0xC0);
    VORTEX_EXPECT_EQ(c[10], uint8_t{0xC3});
}

VORTEX_TEST(x64_memory_operand_golden) {
    CodeBuffer buf;
    x64::Assembler a(buf);
    // mov rax, [rbx + 8] => 48 8B 43 08
    a.mov_reg_mem(x64::Reg::RAX, x64::Mem{x64::Reg::RBX, x64::Reg::RSP, 0, 8});
    // mov [rcx + rax*8], rdx => 48 89 14 C1
    a.mov_mem_reg(x64::Mem{x64::Reg::RCX, x64::Reg::RAX, 3, 0}, x64::Reg::RDX);
    const std::vector<uint8_t>& c = buf.code();
    VORTEX_EXPECT_EQ(c.size(), size_t{4 + 4});
    VORTEX_EXPECT(c[0] == 0x48 && c[1] == 0x8B && c[2] == 0x43 && c[3] == 0x08);
    VORTEX_EXPECT(c[4] == 0x48 && c[5] == 0x89 && c[6] == 0x14 && c[7] == 0xC1);
}

VORTEX_TEST(x64_branch_placeholder_binding) {
    CodeBuffer buf;
    x64::Assembler a(buf);
    const size_t at = a.placeholder_jmp();   // E9 rel32
    a.bind_placeholder(at);                  // binds to next instruction (0 rel)
    const size_t at2 = a.placeholder_call();
    a.bind_placeholder(at2);
    const std::vector<uint8_t>& c = buf.code();
    VORTEX_EXPECT_EQ(c.size(), size_t{5 + 5});
    VORTEX_EXPECT_EQ(c[1], uint8_t{0});  // rel32 = 0: target is next insn
    VORTEX_EXPECT_EQ(c[6], uint8_t{0});
}

VORTEX_TEST(wx_memory_publish_and_execute) {
    // End-to-end W^X: emit "mov eax, 42; ret" in RW pages, flip to RX, call it.
    auto mem = infra::WritableCodeMemory::allocate(64);
    VORTEX_EXPECT(mem.has_value());
    if (!mem) return;
    // B8 2A 00 00 00 = mov eax, 42 (32-bit operand form); C3 = ret
    const uint8_t code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    mem->write(code, 0);
    auto published = mem->publish();
    VORTEX_EXPECT(published.has_value());
    if (!published) return;
    VORTEX_EXPECT(mem->published());
    using Fn = int (*)();
    const Fn fn = reinterpret_cast<Fn>(mem->data());
    VORTEX_EXPECT_EQ(fn(), 42);
}

VORTEX_TEST(constant_blinding_roundtrip) {
    const uint64_t mask = infra::ConstantBlinding::random_mask();
    infra::ConstantBlinding blinding(mask);
    const uint64_t value = 0x1234567890ABCDEFull;
    const uint64_t blinded = blinding.blind(value);
    VORTEX_EXPECT(blinded != value);
    VORTEX_EXPECT_EQ(blinding.unblind(blinded), value);
    // Two random masks differ (overwhelming probability).
    VORTEX_EXPECT(mask != infra::ConstantBlinding::random_mask());
    // Strict policy: default threshold blinds everything; a positive band
    // exempts only small immediates.
    VORTEX_EXPECT(blinding.should_blind(0));
    infra::ConstantBlinding banded(mask);
    banded.set_blind_threshold(100);
    VORTEX_EXPECT(!banded.should_blind(50));
    VORTEX_EXPECT(banded.should_blind(1000));
    VORTEX_EXPECT(banded.should_blind(-1000));
}

VORTEX_TEST(code_cache_segmented_allocation) {
    infra::CodeCache cache(4096);
    uint8_t code[16] = {};
    code[0] = 0xC3;  // ret
    auto a1 = cache.allocate(infra::CodeSegmentKind::Baseline, Tier::J1, 1, code);
    auto a2 = cache.allocate(infra::CodeSegmentKind::Optimized, Tier::J3, 2, code);
    VORTEX_EXPECT(a1.has_value());
    VORTEX_EXPECT(a2.has_value());
    if (a1 && a2) {
        VORTEX_EXPECT(a2->offset >= a1->offset + a1->size);
        VORTEX_EXPECT(cache.code_of(*a1) != nullptr);
        VORTEX_EXPECT_EQ(cache.code_of(*a1)[0], uint8_t{0xC3});
        VORTEX_EXPECT_EQ(cache.allocation_count(), size_t{2});
    }
}

VORTEX_TEST(cpu_detection_reports_something) {
    const infra::CpuInfo info = infra::detect_cpu();
    const infra::FeatureLevel level = infra::feature_level(info.features);
    VORTEX_EXPECT(infra::feature_level_name(level) != nullptr);
    // Feature bits must not collide: a CPU reporting AVX-512F must also pass
    // the monotone level query (guards against the bit-mapping regressions).
    if (info.features & (1ull << 7)) {
        VORTEX_EXPECT(infra::feature_level(info.features) ==
                      infra::FeatureLevel::Avx512);
    }
}
