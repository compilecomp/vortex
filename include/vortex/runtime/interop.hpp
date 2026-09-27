// The Interop Message Protocol (docs/interop-protocol.md).
//
// A uniform, capability-gated message surface for driving guest objects
// from any language port: one Send dispatcher plus a canonical message
// set, gated by UGB capability bits, with hot messages holding dedicated
// UGB opcodes (POLY_EXECUTE / POLY_READ / POLY_WRITE / POLY_SEND) so
// J1-J4 can devirtualize and inline them.
//
// Status: spec surface (docs/roadmap.md M3/M4) — the message model,
// capability bits and vtable contract are fixed here so the devirt
// graph, the structural hasher and the language ports can be built
// against one canonical shape.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/support/containers.hpp"
#include "vortex/support/result.hpp"
#include "vortex/support/tagged_value.hpp"

namespace vortex::runtime::interop {

// ---- capability bits (UGB loader validation) --------------------------------

/// Capability groups for the interop message surface. The loader maps
/// each POLY_* opcode to a group and rejects modules that emit a message
/// whose group bit is absent from the module capability mask (Rule 3:
/// negotiation at load, never silent misexecution).
enum InteropCapability : uint32_t {
    CAP_INTEROP_MEMBERS    = 1 << 0,  // structure messages
    CAP_INTEROP_EXECUTE    = 1 << 1,  // EXECUTE / INSTANTIATE
    CAP_INTEROP_ARRAY      = 1 << 2,  // array element messages
    CAP_INTEROP_PRIMITIVES = 1 << 3,  // unbox messages
    CAP_INTEROP_POINTER    = 1 << 4,  // FFI pointer messages
    CAP_INTEROP_EXCEPTION  = 1 << 5,  // exception messages
};

// ---- message ids (closed set; unknown ids are safely rejectable) -------------

enum InteropMessage : uint16_t {
    MSG_HAS_MEMBERS = 0x00,
    MSG_READ_MEMBER = 0x01,
    MSG_WRITE_MEMBER = 0x02,
    MSG_REMOVE_MEMBER = 0x03,
    MSG_MEMBERS = 0x04,
    MSG_IS_MEMBER_READABLE = 0x05,
    MSG_IS_MEMBER_WRITABLE = 0x06,

    MSG_IS_EXECUTABLE = 0x10,
    MSG_EXECUTE = 0x11,
    MSG_IS_INSTANTIABLE = 0x12,
    MSG_INSTANTIATE = 0x13,

    MSG_HAS_ARRAY_ELEMENTS = 0x20,
    MSG_READ_ARRAY_ELEMENT = 0x21,
    MSG_WRITE_ARRAY_ELEMENT = 0x22,
    MSG_ARRAY_SIZE = 0x23,

    MSG_IS_BOOLEAN = 0x30,
    MSG_AS_BOOLEAN = 0x31,
    MSG_IS_NUMBER = 0x32,
    MSG_AS_NUMBER = 0x33,
    MSG_IS_STRING = 0x34,
    MSG_AS_STRING = 0x35,
    MSG_IS_NULL = 0x36,

    MSG_IS_POINTER = 0x40,
    MSG_AS_POINTER = 0x41,

    MSG_IS_EXCEPTION = 0x50,
    MSG_THROW = 0x51,
    MSG_GET_EXCEPTION_TYPE = 0x52,
};

/// The capability group a message belongs to (loader validation table).
/// Keyed by the InteropMessage enum — one table, no duplicated literals
/// (Rule 72: the ids live in exactly one place).
constexpr InteropCapability capability_of(uint16_t message_id) noexcept {
    switch (static_cast<InteropMessage>(message_id)) {
    case MSG_HAS_MEMBERS:
    case MSG_READ_MEMBER:
    case MSG_WRITE_MEMBER:
    case MSG_REMOVE_MEMBER:
    case MSG_MEMBERS:
    case MSG_IS_MEMBER_READABLE:
    case MSG_IS_MEMBER_WRITABLE:
        return CAP_INTEROP_MEMBERS;
    case MSG_IS_EXECUTABLE:
    case MSG_EXECUTE:
    case MSG_IS_INSTANTIABLE:
    case MSG_INSTANTIATE:
        return CAP_INTEROP_EXECUTE;
    case MSG_HAS_ARRAY_ELEMENTS:
    case MSG_READ_ARRAY_ELEMENT:
    case MSG_WRITE_ARRAY_ELEMENT:
    case MSG_ARRAY_SIZE:
        return CAP_INTEROP_ARRAY;
    case MSG_IS_BOOLEAN:
    case MSG_AS_BOOLEAN:
    case MSG_IS_NUMBER:
    case MSG_AS_NUMBER:
    case MSG_IS_STRING:
    case MSG_AS_STRING:
    case MSG_IS_NULL:
        return CAP_INTEROP_PRIMITIVES;
    case MSG_IS_POINTER:
    case MSG_AS_POINTER:
        return CAP_INTEROP_POINTER;
    case MSG_IS_EXCEPTION:
    case MSG_THROW:
    case MSG_GET_EXCEPTION_TYPE:
        return CAP_INTEROP_EXCEPTION;
    default:
        return static_cast<InteropCapability>(0);  // unknown -> unsupported
    }
}

// ---- per-language message vtable ----------------------------------------------
//
// One flat vtable per registered language port; a null slot means "this
// language does not support the message" — the dispatcher returns
// UNSUPPORTED (safe fallback, Rule 3). Slots are plain function pointers
// registered once at language load (@cold); the hot path is the
// dispatcher plus the JIT specializations (docs/interop-protocol.md
// section 6). All receivers/arguments flow as tagged values; member
// access uses member_idx tokens, never strings (Rule 5).

struct InteropVTable {
    // Structure: bool(receiver, member_idx)
    uint32_t (*has_members)(void* recv, uint32_t member_idx);
    // Read/Write: value bits out / in; 0 return = UNSUPPORTED or failure
    uint64_t (*read_member)(void* recv, uint32_t member_idx);
    uint32_t (*write_member)(void* recv, uint32_t member_idx, uint64_t value);
    uint32_t (*remove_member)(void* recv, uint32_t member_idx);
    // EXECUTE: args window + ret out; 0 return = success
    int64_t (*execute)(void* recv, const void* args, uint32_t argc,
                       void* ret);
    // Array: element read/write by index
    uint64_t (*read_array)(void* recv, uint32_t index);
    uint32_t (*write_array)(void* recv, uint32_t index, uint64_t value);
    // Unbox: canonical primitive extraction
    uint64_t (*unbox)(void* recv, uint16_t message_id);
};

// ---- language registry (@cold registration, hot reads by id) -----------------
//
// One registry per runtime. Language ports register once at load (docs
// section 4: plain function pointers, @cold); the hot path never touches
// strings — receivers resolve to a language id through their klass word
// (Rule 5: token-keyed identity), and the tiers consume the recorded ids
// from the IC/profile slots.

/// How a port's wrappers behave for the optimizing tiers. NativeObjects
/// ports give member_idx = vortex field-slot semantics, which is the
/// speculation license the graph builder needs to lower POLY_READ/WRITE
/// into guarded raw field accesses (docs/interop-protocol.md section 6:
/// the devirt graph inlines the handler; for a native port the handler IS
/// the field access). Foreign ports keep every message on the generic
/// dispatch — no JIT speculation without provable handler semantics.
enum class PortKind : uint8_t { Foreign, NativeObjects };

class InteropRegistry {
public:
    /// Registers one language port. Returns the assigned language id
    /// (1-based; 0 is the invalid id). @cold: single-threaded registration.
    /// Lifecycle: duplicate names are accepted (ids stay the identity);
    /// name lookup resolves the FIRST registration deterministically.
    uint16_t register_language(std::string_view name,
                               const InteropVTable& vtable,
                               uint32_t capabilities,
                               PortKind kind = PortKind::Foreign);

    /// Binds the wrapper klass address `klass` to `language_id` — the
    /// dispatcher's receiver->language resolution. @cold.
    ///
    /// Binding contract (the JIT port-kind license's invalidation
    /// dependency, Rules 30/31): a binding is FROZEN once made. Re-binding
    /// the same (klass, language) pair is idempotent; re-binding a klass
    /// to a DIFFERENT language is a named error — installed J2/J3 code
    /// speculates on "klass K is a NativeObjects port" with a klass
    /// identity guard but NO runtime port re-check, so a silent rebind
    /// would let stale compiled code execute raw field accesses with the
    /// wrong message semantics (Rule 3: never silent misexecution).
    /// Invalid arguments (bad language id, null klass) are named errors
    /// too — no silent no-ops (Rule 76).
    support::Result<void> bind_klass(uint16_t language_id, const void* klass);

    uint16_t language_id(std::string_view name) const noexcept;
    const char* language_name(uint16_t id) const noexcept;
    uint32_t capabilities_of(uint16_t id) const noexcept;
    const InteropVTable* vtable_of(uint16_t id) const noexcept;
    /// Receiver resolution: the klass word of an interop wrapper -> the
    /// language id driving it; 0 when the klass is not a registered port.
    uint16_t language_of_klass(const void* klass) const noexcept;
    /// The port kind behind a bound wrapper klass (Foreign when unbound).
    PortKind port_kind_of_klass(const void* klass) const noexcept;
    size_t language_count() const noexcept { return languages_.size(); }

private:
    struct Language {
        std::string name;
        InteropVTable vtable{};
        uint32_t capabilities = 0;
        PortKind kind = PortKind::Foreign;
    };
    support::FlatHashMap<const void*, uint16_t> klass_to_language_;
    std::vector<Language> languages_;  // index i holds language id i+1
};

// ---- tagged dispatch (the surface T0's POLY_* handlers call) -----------------
//
// All functions take the registry explicitly (no hidden globals — CEM-26
// section 6), resolve the receiver's language through its klass word, and
// return named failures for: not-an-object, unregistered klass, missing
// capability, and null vtable slot = UNSUPPORTED (docs section 4, Rule 3).

/// kErrInteropUnsupported propagation: a dedicated Result failure the ports
/// surface uniformly (docs section 7). The message string is stable so
/// tests can assert on it.
constexpr std::string_view kInteropUnsupportedMessage =
    "interop: message unsupported by receiver language";

support::Result<TaggedValue> dispatch_read_member(const InteropRegistry&,
                                                  const TaggedValue& recv,
                                                  uint32_t member_idx);
support::Result<void> dispatch_write_member(const InteropRegistry&,
                                            const TaggedValue& recv,
                                            uint32_t member_idx,
                                            const TaggedValue& value);
support::Result<TaggedValue> dispatch_execute(const InteropRegistry&,
                                              const TaggedValue& recv,
                                              std::span<const TaggedValue> args);
support::Result<TaggedValue> dispatch_send(const InteropRegistry&,
                                           uint16_t message_id,
                                           const TaggedValue& recv,
                                           std::span<const TaggedValue> args);
/// The language id a POLY_* site records into its IC/profile slot
/// (0 = unresolvable — the tiers then never speculate on this receiver).
uint16_t receiver_language(const InteropRegistry&, const TaggedValue& recv);

}  // namespace vortex::runtime::interop
