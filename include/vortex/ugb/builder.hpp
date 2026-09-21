// Programmatic UGB module construction — used by language frontends (any guest
// language port), tests and the text assembler (docs/ugb.md, src/ugb).
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::ugb {

using support::Diagnostic;
using support::Result;

/// Fluent single-method builder. Instructions are appended in order; branch
/// targets are resolved from named labels at `finish()`.
class MethodBuilder {
public:
    MethodBuilder(UGBModule& module, std::string name, uint16_t register_count,
                  uint16_t arg_count);

    /// Emits one instruction. Returns the instruction's bytecode offset.
    size_t emit(Op opcode, uint16_t dst = 0,
                std::initializer_list<uint16_t> srcs = {}, bool has_meta = false,
                uint32_t meta = 0);

    // ---- convenience emitters ------------------------------------------------
    size_t const_i64(uint16_t dst, int64_t value);        // pool-interned
    size_t const_i32(uint16_t dst, int32_t value);        // immediate
    size_t const_f64(uint16_t dst, double value);
    size_t const_null(uint16_t dst);
    size_t const_true(uint16_t dst);
    size_t const_false(uint16_t dst);
    size_t jump(std::string_view label);
    size_t jump_true(uint16_t cond, std::string_view label);
    size_t jump_false(uint16_t cond, std::string_view label);
    size_t call_direct(uint16_t dst_result, uint16_t arg_base, uint16_t argc,
                       std::string_view callee);
    size_t call_builtin(uint16_t dst_result, uint16_t arg_base, uint16_t argc,
                        std::string_view builtin);
    size_t ret(uint16_t value);

    void bind_label(std::string_view label);
    size_t current_offset() const noexcept { return code_.size(); }
    size_t instruction_count() const noexcept { return instruction_count_; }

    /// Resolves labels and appends the method to the module.
    Result<void> finish();

private:
    struct LabelRef {
        size_t patch_offset = 0;
        std::string label;
    };

    UGBModule& module_;
    UGBMethod method_;
    std::vector<uint8_t> code_;
    std::map<std::string, size_t> labels_;
    std::vector<LabelRef> label_refs_;
    size_t instruction_count_ = 0;
};

/// Text assembler (docs/ugb.md; user-selected test vehicle). Grammar:
///
///   .language "vx"
///   .method <name>(regs=<n>, args=<n>)
///       <instruction>;?     # 'Add.I32 v3, v1, v2' or alias 'ADD_I32 v3, v1, v2'
///   .end
///   <label>:                # branch target
///   .class <name>
///   .field <name> in <class>
///   .builtin <name>
///
/// Comments: '#' to end of line. Constants: 42, -7, 3.14, "text".
/// The assembler owns its module and returns it on success.
Result<UGBModule> assemble_module(std::string_view source);

}  // namespace vortex::ugb
