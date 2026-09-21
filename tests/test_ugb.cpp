// UGB module codec, assembler, verifier and disassembler tests.
#include "vortex_test.hpp"

#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/disassembler.hpp"
#include "vortex/ugb/verifier.hpp"

using namespace vortex;
using namespace vortex::ugb;

VORTEX_TEST(opcode_names_roundtrip) {
    // Every opcode's canonical name parses back to the same opcode.
    for (uint16_t i = 1; i < kOpcodeCount; ++i) {
        const Op op = static_cast<Op>(i);
        const Op back = parse_opcode(opcode_name(op));
        if (back != op) {
            VORTEX_EXPECT_EQ(static_cast<int>(back), static_cast<int>(op));
            break;
        }
    }
    // Spec alias normalization: "ADD_I32" and "Add.I32" agree.
    VORTEX_EXPECT_EQ(parse_opcode("ADD_I32"), Op::ADD_I32);
    VORTEX_EXPECT_EQ(parse_opcode("Add.I32"), Op::ADD_I32);
    VORTEX_EXPECT_EQ(parse_opcode("nope.nope"), Op::ILLEGAL);
}

VORTEX_TEST(speculative_canonical_mapping) {
    VORTEX_EXPECT(is_speculative(Op::ADD_I32));
    VORTEX_EXPECT(!is_speculative(Op::ADD_ANY));
    VORTEX_EXPECT_EQ(canonical_form(Op::ADD_I32), Op::ADD_ANY);
    VORTEX_EXPECT_EQ(canonical_form(Op::GET_FIELD_SHAPE), Op::GET_FIELD);
    VORTEX_EXPECT_EQ(canonical_form(Op::RETURN), Op::RETURN);
}

VORTEX_TEST(builder_and_labels) {
    UGBModule module;
    MethodBuilder b(module, "add10", 4, 1);
    b.const_i32(1, 10);
    b.emit(Op::ADD_ANY, 2, {0, 1});
    b.ret(2);
    auto ok = b.finish();
    VORTEX_EXPECT(ok.has_value());
    VORTEX_EXPECT_EQ(module.method_table.size(), size_t{1});
    auto verified = verify_module(module);
    VORTEX_EXPECT(verified.has_value());
}

VORTEX_TEST(assembler_fib_module) {
    const char* src = R"(
.method fib(regs=7, args=1)
    Const.I32 v1, 0
    Const.I32 v2, 1
    Const.I32 v3, 1
loop:
    Const.I32 v4, 0
    Eq.I64 v5, v0, v4
    JumpTrue v5, done
    Add.I64 v6, v1, v2
    Move v1, v2
    Move v2, v6
    Sub.I64 v0, v0, v3
    Jump loop
done:
    Return v1
.end
.method main(regs=3, args=0)
    Const.I32 v0, 25
    Call.Direct v1, v0, 1, fib
    Return v1
.end
)";
    auto module = assemble_module(src);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    VORTEX_EXPECT_EQ(module->method_table.size(), size_t{2});
    auto verified = verify_module(*module);
    VORTEX_EXPECT(verified.has_value());
    if (!verified) std::printf("  verify: %s\n", verified.error().message.c_str());
}

VORTEX_TEST(binary_encode_decode_roundtrip) {
    auto module = assemble_module(R"(
.method f(regs=4, args=2)
    ADD_I32 v2, v0, v1
    Return v2
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    const std::vector<uint8_t> binary = encode_module(*module);
    UGBModule decoded;
    DecodeErrorInfo err;
    VORTEX_EXPECT(decode_module(binary.data(), binary.size(), decoded, err));
    VORTEX_EXPECT_EQ(decoded.method_table.size(), size_t{1});
    VORTEX_EXPECT_EQ(decoded.method_table[0].name, "f");
    VORTEX_EXPECT_EQ(decoded.method_table[0].arg_count, uint16_t{2});
    VORTEX_EXPECT_EQ(decoded.method_table[0].code.size(),
                     module->method_table[0].code.size());
    VORTEX_EXPECT(decoded.method_table[0].code == module->method_table[0].code);
}

VORTEX_TEST(verifier_rejects_bad_modules) {
    // Register out of range.
    auto module = assemble_module(R"(
.method bad(regs=2, args=1)
    ADD_ANY v0, v1, v9
    Return v0
.end
)");
    VORTEX_EXPECT(module.has_value());
    auto res = verify_module(*module);
    VORTEX_EXPECT(!res.has_value());
}

VORTEX_TEST(verifier_rejects_branch_into_middle) {
    // Jump target 8 is not an instruction start: Const.I32 occupies 0..9.
    UGBModule module;
    MethodBuilder b(module, "bad", 2, 0);
    b.const_i32(0, 1);
    b.emit(Op::JUMP, 0, {}, true, 8);
    b.const_i32(1, 2);
    b.ret(0);
    auto ok = b.finish();
    VORTEX_EXPECT(ok.has_value());
    auto res = verify_module(module);
    VORTEX_EXPECT(!res.has_value());
}

VORTEX_TEST(verifier_rejects_call_window_oob) {
    // Regression: `Call.Direct v0, v7, 5, f` with regs=8 reads R[7..11], four
    // TaggedValues past the frame. The call argument window must be checked
    // (arg_base + argc <= register_count), and argc must not be treated as a
    // register operand.
    UGBModule module;
    module.intern_method("f");
    MethodBuilder victim(module, "victim", 8, 0);
    victim.emit(Op::CALL_DIRECT, 0, {7, 5}, true, 1);  // window R[7..11] OOB
    victim.ret(0);
    VORTEX_EXPECT(victim.finish().has_value());
    auto res = verify_module(module);
    VORTEX_EXPECT(!res.has_value());

    // A well-formed call passes: arg window R[1..2] with regs=8.
    UGBModule ok_module;
    ok_module.intern_method("f");
    MethodBuilder ok_b(ok_module, "ok", 8, 0);
    ok_b.emit(Op::CALL_DIRECT, 0, {1, 2}, true, 1);
    ok_b.ret(0);
    VORTEX_EXPECT(ok_b.finish().has_value());
    auto ok_res = verify_module(ok_module);
    VORTEX_EXPECT(ok_res.has_value());
}

VORTEX_TEST(verifier_rejects_bad_tokens) {
    // Constant pool index out of range.
    UGBModule module;
    MethodBuilder b(module, "f", 2, 0);
    b.emit(Op::CONST_I64, 0, {}, true, 999);
    b.ret(0);
    VORTEX_EXPECT(b.finish().has_value());
    auto res = verify_module(module);
    VORTEX_EXPECT(!res.has_value());

    // Method token out of range on a direct call.
    UGBModule m2;
    MethodBuilder b2(m2, "g", 4, 0);
    b2.emit(Op::CALL_DIRECT, 0, {0, 1}, true, 42);
    b2.ret(0);
    VORTEX_EXPECT(b2.finish().has_value());
    auto res2 = verify_module(m2);
    VORTEX_EXPECT(!res2.has_value());
}

VORTEX_TEST(disassembler_renders_text) {
    auto module = assemble_module(R"(
.method f(regs=4, args=2)
    Const.I32 v2, 10
    ADD_I32 v3, v0, v1
    Return v3
.end
)");
    VORTEX_EXPECT(module.has_value());
    const std::string text = disassemble_module(*module);
    VORTEX_EXPECT(text.find(".method f(regs=4, args=2)") != std::string::npos);
    VORTEX_EXPECT(text.find("Add.I32") != std::string::npos);
    VORTEX_EXPECT(text.find(".end") != std::string::npos);
}
