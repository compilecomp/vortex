// UGB module + method containers (docs/ugb.md sections 3 and 5).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "vortex/support/tagged_value.hpp"
#include "vortex/ugb/opcode.hpp"

namespace vortex::ugb {

/// Constant pool entry. TaggedValue constants are materialized at load time.
struct Constant {
    enum class Kind : uint8_t { Int64, Float64, String, MethodRef };
    Kind kind = Kind::Int64;
    int64_t i64 = 0;
    double f64 = 0.0;
    std::string str;
};

/// Token tables. Tokens are u32 indices into module-level tables
/// (docs/ugb.md section 4.4).
struct FieldTokenInfo {
    std::string name;
    uint32_t klass_token = 0;
};

struct ClassTokenInfo {
    std::string name;
};

struct MethodTokenInfo {
    std::string name;
};

struct BuiltinTokenInfo {
    std::string name;
};

/// Interpreter-maintained profile slot (docs/tier-t0.md section 6).
struct ProfileSlot {
    uint64_t execution_count = 0;
    uint32_t failure_count = 0;
    uint8_t type_ring_size = 0;
    uint8_t type_ring[4] = {0, 0, 0, 0};  // observed type tags
    uint32_t branch_counts[2] = {0, 0};

    void record_execution() noexcept { ++execution_count; }
    void record_failure() noexcept { ++failure_count; }
    void record_branch(bool taken) noexcept { ++branch_counts[taken ? 1 : 0]; }
};

/// Inline cache states and slot (docs/tier-t0.md section 3).
enum class IcState : uint8_t { Uninitialized, Monomorphic, Polymorphic, Megamorphic };

struct IcEntry {
    uint32_t klass_or_shape_id = 0;
    uint32_t target = 0;  // field slot / method id / offset
};

struct IcSlot {
    IcState state = IcState::Uninitialized;
    IcEntry mono{};
    IcEntry poly[4]{};
    uint8_t poly_count = 0;
    uint32_t failure_count = 0;

    /// Atomic, thread-safe transition (docs/tier-t0.md: IC updates are atomic).
    /// Returns the entry hit, or nullptr on miss (caller takes slow path).
    const IcEntry* lookup(uint32_t klass_id) const noexcept;
    void record_hit(uint32_t klass_id, uint32_t target) noexcept;
};

/// A single UGB method (function).
struct UGBMethod {
    std::string name;
    uint16_t register_count = 0;
    uint16_t arg_count = 0;
    uint32_t id = 0;

    /// Encoded instruction stream (fixed-width encoding, docs/ugb.md section 8).
    std::vector<uint8_t> code;

    /// Runtime tables, allocated per method on first execution (not serialized
    /// in M0 — docs/ugb.md sections 5 and 14).
    std::vector<ProfileSlot> profiles;
    std::vector<IcSlot> ics;
    bool runtime_tables_ready = false;

    void ensure_runtime_tables(size_t instruction_count) {
        if (runtime_tables_ready) return;
        profiles.resize(instruction_count);
        ics.resize(instruction_count);
        runtime_tables_ready = true;
    }
};

/// The module: constants, token tables and methods.
struct UGBModule {
    uint16_t version_major = 0;
    uint16_t version_minor = 1;
    uint32_t language_id = 0;
    std::string language_name = "vx";

    std::vector<Constant> constants;
    std::vector<ClassTokenInfo> classes;
    std::vector<FieldTokenInfo> fields;
    std::vector<MethodTokenInfo> methods;  // method tokens (token 0 = none)
    std::vector<BuiltinTokenInfo> builtins;
    std::vector<UGBMethod> method_table;   // methods in definition order

    // ---- token interning -----------------------------------------------------
    uint32_t intern_class(const std::string& name);
    uint32_t intern_field(const std::string& name, uint32_t klass_token);
    uint32_t intern_method(const std::string& name);
    uint32_t intern_builtin(const std::string& name);
    uint32_t intern_i64(int64_t v);
    uint32_t intern_f64(double v);
    uint32_t intern_string(const std::string& s);

    int32_t find_method(const std::string& name) const noexcept;

    /// Materializes a constant pool entry as a tagged value.
    TaggedValue materialize_constant(uint32_t index) const;
};

// ---- binary encoding (docs/ugb.md sections 8 and 18) ---------------------------

/// Encodes a module to the compact distribution format.
std::vector<uint8_t> encode_module(const UGBModule& module);

/// Decodes a module. Fails with DecodeError on malformed input.
struct DecodeErrorInfo {
    std::string message;
    size_t offset = 0;
};
bool decode_module(const uint8_t* data, size_t size, UGBModule& out,
                   DecodeErrorInfo& error);

/// One decoded instruction (wide internal form).
struct Instruction {
    Op opcode = Op::ILLEGAL;
    uint8_t flags = 0;
    uint16_t dst = 0;
    std::vector<uint16_t> srcs;
    bool has_meta = false;
    uint32_t meta = 0;
};

/// Streaming decoder over an encoded code stream.
class InstructionStream {
public:
    InstructionStream(const uint8_t* data, size_t size) noexcept
        : data_(data), size_(size) {}

    /// Decodes the instruction at `offset`; advances `offset` past it.
    bool decode_at(size_t& offset, Instruction& out) const noexcept;

    /// Returns the absolute bytecode offset of the instruction starting at or
    /// after `from` that is `n` instructions ahead (used by the adaptive
    /// rewriter for superinstruction fusion).
    bool next_offset(size_t from, uint32_t n, size_t& out) const noexcept;

    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }

private:
    const uint8_t* data_;
    size_t size_;
};

/// Appends one instruction to an encoded stream. `meta` is written when
/// `has_meta` is set.
void append_instruction(std::vector<uint8_t>& code, Op opcode, uint16_t dst,
                        std::initializer_list<uint16_t> srcs, bool has_meta,
                        uint32_t meta);

}  // namespace vortex::ugb
