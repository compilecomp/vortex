// UGB module + method containers (docs/ugb.md sections 3 and 5).
//
// Format laws implemented here (Compiler Laws & Architecture Specification):
//   Rule 3  — every method declares required capabilities; the engine rejects
//             methods requiring unsupported capabilities safely.
//   Rule 5  — all identifiers are tokenized; runtime caches are keyed by IDs,
//             never by strings (string lookups are cold-path only).
//   Rule 6  — extensions are namespaced ("extension.<language>.<feature>")
//             and versioned.
//   Rule 8  — every dynamic/speculative site has a stable site ID derived
//             from (method id, instruction ordinal).
//   Rule 10 — artifacts record UGB version, capability set, extension
//             namespaces, guest language ID, runtime ABI version and
//             metadata schema version. Incompatible versions are rejected.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vortex/support/containers.hpp"
#include "vortex/support/tagged_value.hpp"
#include "vortex/ugb/opcode.hpp"

namespace vortex::ugb {

// ---- Rule 10: explicit artifact versioning ---------------------------------

inline constexpr uint16_t kUgbVersionMajor = 0;
// v2 adds: runtime ABI + metadata schema versions, extension table, module
// capability mask, per-method capability lists. v1 payloads decode with
// defaults (empty capability set); incompatible majors are rejected.
// v3 adds: per-field `declared` flag — distinguishes fields introduced by a
// `.field` directive (they shape the klass layout) from field tokens created
// by bare instruction references (accessing one is the canonical unresolved-
// field error, never an out-of-bounds slot). Older minors decode with
// declared=true (the v2 layout contract).
inline constexpr uint16_t kUgbVersionMinor = 3;
inline constexpr uint32_t kRuntimeAbiVersion = 1;
inline constexpr uint32_t kMetadataSchemaVersion = 1;

// ---- Rule 3: capability negotiation ----------------------------------------

/// Capabilities a method may require. Closed set: unknown values (>= _COUNT,
/// introduced by newer producers) make the method safely rejectable, never
/// silently misexecutable.
enum class Capability : uint8_t {
    TypedArithmetic,  // speculative typed smi arithmetic forms
    Float64,          // boxed double values and float ops
    Arrays,           // New.Array / Array.Get / Array.Set / Array.Length
    Classes,          // classes, fields, New.Object
    VirtualCalls,     // Call.Virtual dispatch through klass method tables
    BuiltinCalls,     // Call.Builtin against runtime-registered builtins
    GuardedAccess,    // Check.* guard family
    FFI,              // native interop (host must opt in)
    Extensions,       // namespaced extension instructions
    Debug,            // DEBUG_SRCPOS / DEBUG_TRAP observability
    _COUNT,
};

const char* capability_name(Capability c) noexcept;
bool is_valid_capability(uint8_t raw) noexcept;

/// Bitset of capabilities supported by an engine/runtime (Rule 3: the runtime
/// advertises what it supports; silent misexecution is forbidden).
class CapabilitySet {
public:
    constexpr CapabilitySet& add(Capability c) noexcept {
        bits_ |= mask(c);
        return *this;
    }
    constexpr bool supports(Capability c) const noexcept {
        return (bits_ & mask(c)) != 0;
    }
    /// A raw capability id from an artifact; unknown ids are never supported.
    constexpr bool supports_raw(uint8_t raw) const noexcept {
        return is_valid_capability(raw) &&
               (bits_ & (1u << raw)) != 0;
    }
    constexpr uint32_t raw() const noexcept { return bits_; }
    static constexpr CapabilitySet from_raw(uint32_t bits) noexcept {
        CapabilitySet s;
        s.bits_ = bits;
        return s;
    }

private:
    static constexpr uint32_t mask(Capability c) noexcept {
        return 1u << static_cast<unsigned>(c);
    }
    uint32_t bits_ = 0;
};

/// The capability set the T0 engine advertises. JIT tiers widen this in
/// later milestones; a tier must never claim a capability it cannot execute
/// (Rule 3: unsupported -> fallback hook, interpreter hook, safe reject, or
/// T0 — never silent misexecution).
CapabilitySet engine_advertised_capabilities() noexcept;

// ---- Rule 6: namespaced, versioned extensions -------------------------------

struct ExtensionRef {
    std::string name;  // "extension.<language>.<feature>"
    uint32_t version = 1;
};

/// Rejects names that do not follow the extension namespace grammar.
bool is_valid_extension_name(std::string_view name) noexcept;

// ---- Rule 8: stable site IDs ------------------------------------------------

constexpr uint64_t kNoSite = 0xFFFFFFFFFFFFFFFFull;

/// Stable across runs for a given (module, bytecode version): method ids are
/// assigned by the producer and the module hash binds the bytecode version.
/// Every profile slot, IC slot and future speculative site is identified by
/// this ID in telemetry and deopt records.
constexpr uint64_t make_site_id(uint32_t method_id,
                                uint32_t instruction_index) noexcept {
    return (static_cast<uint64_t>(method_id) << 32) |
           static_cast<uint64_t>(instruction_index);
}

// ---- constant pool + token tables ------------------------------------------

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
    /// Set by the `.field` directive (text) or the v3 binary flag. Only
    /// declared fields contribute to their owner klass's layout; a token
    /// created solely by an instruction reference never does, so access
    /// resolves through find_field -> miss -> canonical unresolved-field
    /// error (the helper contract, ADR-005).
    bool declared = false;
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

// ---- runtime state (never serialized) ---------------------------------------

// CEM-26 section 2: profile/IC semantic-domain constants. The type ring
// depth and the branch-outcome arity are spec-visible (docs/tier-t0.md
// section 6); the poly capacity is the mono→poly→mega state machine bound.
inline constexpr uint8_t kIcPolyCapacity = 4;
inline constexpr uint8_t kProfileTypeRingDepth = 4;
inline constexpr uint8_t kBranchOutcomeCount = 2;  // not-taken, taken

/// Interpreter-maintained profile slot (docs/tier-t0.md section 6).
/// Layout audit (CEM-26 section 9): one slot lives per instruction in a
/// dense vector; 32 bytes = 1/2 cache line. SoA was considered and rejected:
/// record_execution/record_failure/record_branch fire together on the same
/// instruction execution, so AoS keeps the working set line-local.
struct ProfileSlot {
    uint64_t execution_count = 0;
    uint32_t failure_count = 0;
    uint8_t type_ring_size = 0;
    uint8_t type_ring[kProfileTypeRingDepth] = {0, 0, 0, 0};  // observed tags
    uint32_t branch_counts[kBranchOutcomeCount] = {0, 0};

    void record_execution() noexcept { ++execution_count; }
    void record_failure() noexcept { ++failure_count; }
    void record_branch(bool taken) noexcept { ++branch_counts[taken ? 1 : 0]; }
};

static_assert(sizeof(ProfileSlot) == 32,
              "ProfileSlot is one dense entry per instruction; growth here "
              "is per-instruction cache traffic");

/// Inline cache states and slot (docs/tier-t0.md section 3).
///
/// Single-mutator contract: in M0 each IcSlot is owned by exactly one
/// interpreter thread; cross-thread publication and atomic IC patching arrive
/// with the J1/J2 code-installation pipeline (Rule 89). Lock-free updates are
/// then expressed with release/acquire stores on the state word (CEM-26
/// section 14: never seq_cst — publication visibility suffices).
enum class IcState : uint8_t { Uninitialized, Monomorphic, Polymorphic, Megamorphic };

struct IcEntry {
    uint32_t klass_or_shape_id = 0;
    uint32_t target = 0;  // field slot / method id / offset
};

/// 48 bytes = 3/4 cache line. Member order is a layout decision (CEM-26
/// section 9): the scalars lead so the u32 counters pack without tail
/// padding, and mono/poly entries end line-adjacent for the scan in
/// lookup(). lookup() is the per-field-access hot path: state test + (mono)
/// one compare or (poly) a capacity-bounded scan — a hit never leaves the
/// line.
struct IcSlot {
    IcState state = IcState::Uninitialized;
    uint8_t poly_count = 0;
    uint32_t failure_count = 0;
    IcEntry mono{};
    IcEntry poly[kIcPolyCapacity]{};

    /// Returns the entry hit, or nullptr on miss (caller takes slow path).
    const IcEntry* lookup(uint32_t klass_id) const noexcept;
    void record_hit(uint32_t klass_id, uint32_t target) noexcept;
};

static_assert(sizeof(IcEntry) == 8, "IC entries are two u32 words");
static_assert(sizeof(IcSlot) == 8 + 8 * kIcPolyCapacity + 8,
              "IcSlot must pack to 48 bytes (scalars first, no tail padding); "
              "the poly scan must stay line-local");

/// Per-method interpreter state: PC<->index maps and heat counters. Pure
/// integers, so it lives beside the method without a layering inversion.
/// Not serialized; rebuilt on first execution.
struct MethodRuntimeData {
    std::vector<uint32_t> instruction_offsets;  // byte offset per instruction
    std::vector<uint32_t> instruction_index;    // instruction index per byte
    uint32_t invocation_count = 0;              // tiering heat (Rule 22)
    uint32_t backedge_count = 0;                // tiering heat (Rule 22)
    bool offsets_ready = false;
};

// ---- method -----------------------------------------------------------------

struct UGBMethod {
    std::string name;
    uint16_t register_count = 0;
    uint16_t arg_count = 0;
    uint32_t id = 0;

    /// Encoded instruction stream (fixed-width encoding, docs/ugb.md section 8).
    std::vector<uint8_t> code;

    /// Rule 3: capabilities this method requires. Empty = core UGB only.
    /// Values are Capability ordinals; unknown values force safe rejection.
    std::vector<uint8_t> required_capabilities;

    /// Runtime tables, allocated per method on first execution (not
    /// serialized — docs/ugb.md sections 5 and 14).
    std::vector<ProfileSlot> profiles;
    std::vector<IcSlot> ics;
    MethodRuntimeData runtime;
    bool runtime_tables_ready = false;

    void ensure_runtime_tables(size_t instruction_count) {
        if (runtime_tables_ready) return;
        profiles.resize(instruction_count);
        ics.resize(instruction_count);
        runtime_tables_ready = true;
    }

    /// Rule 8: stable site ID for the dynamic site at `instruction_index`.
    uint64_t site_id(uint32_t instruction_index) const noexcept {
        return make_site_id(id, instruction_index);
    }
};

// ---- module -----------------------------------------------------------------

/// Per-module execution caches (Rule 5: keyed by tokens/IDs, not strings).
/// klass_table stores opaque handles owned by the runtime object model —
/// the UGB layer never dereferences them. Not serialized.
struct ModuleRuntimeData {
    std::vector<void*> klass_table;          // class token -> klass handle
    std::vector<int32_t> field_slot;         // field token -> canonical slot
    std::vector<int32_t> builtin_slot;       // builtin token -> registry index
    std::vector<int32_t> method_resolution;  // method token -> method_table idx
    // (klass_id << 32 | method_token) -> method_table index
    support::FlatHashMap<uint64_t, int32_t> virtual_resolution;
    // (klass_id << 32 | field_token) -> field slot on that klass
    support::FlatHashMap<uint64_t, int32_t> field_slot_cache;
    bool ready = false;
};

struct UGBModule {
    uint16_t version_major = kUgbVersionMajor;
    uint16_t version_minor = kUgbVersionMinor;
    uint32_t language_id = 0;
    std::string language_name = "vx";
    uint32_t runtime_abi_version = kRuntimeAbiVersion;      // Rule 10
    uint32_t metadata_schema_version = kMetadataSchemaVersion;  // Rule 10
    std::vector<ExtensionRef> extensions;                    // Rule 6/10
    uint32_t capability_mask = 0;                            // Rule 3/10 (union)

    std::vector<Constant> constants;
    std::vector<ClassTokenInfo> classes;
    std::vector<FieldTokenInfo> fields;
    std::vector<MethodTokenInfo> methods;  // method tokens (token 0 = none)
    std::vector<BuiltinTokenInfo> builtins;
    std::vector<UGBMethod> method_table;   // methods in definition order

    /// Execution caches; rebuilt per engine instance (not serialized).
    ModuleRuntimeData runtime;

    // ---- token interning -----------------------------------------------------
    uint32_t intern_class(const std::string& name);
    uint32_t intern_field(const std::string& name, uint32_t klass_token);
    uint32_t intern_method(const std::string& name);
    uint32_t intern_builtin(const std::string& name);
    uint32_t intern_i64(int64_t v);
    uint32_t intern_f64(double v);
    uint32_t intern_string(const std::string& s);

    int32_t find_method(const std::string& name) const noexcept;

    /// Materializes a constant pool entry as a tagged value. Boxed kinds
    /// (Float64/String/MethodRef) require heap materialization by the engine
    /// (the interpreter handles CONST_F64 itself); this API returns undefined
    /// for them rather than a silently truncated value.
    TaggedValue materialize_constant(uint32_t index) const;
};

// ---- binary encoding (docs/ugb.md sections 8 and 18) ---------------------------

/// Encodes a module to the compact distribution format.
std::vector<uint8_t> encode_module(const UGBModule& module);

/// Decodes a module. Fails with DecodeError on malformed input. Version law
/// (Rule 10): rejects incompatible major versions; decodes older minors with
/// documented defaults.
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
