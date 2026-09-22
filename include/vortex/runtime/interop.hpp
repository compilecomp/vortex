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

/// The capability group a message belongs to (loader validation table).
constexpr InteropCapability capability_of(uint16_t message_id) noexcept {
    switch (message_id) {
    case 0x00:  // HAS_MEMBERS
    case 0x01:  // READ_MEMBER
    case 0x02:  // WRITE_MEMBER
    case 0x03:  // REMOVE_MEMBER
    case 0x04:  // MEMBERS
    case 0x05:  // IS_MEMBER_READABLE
    case 0x06:  // IS_MEMBER_WRITABLE
        return CAP_INTEROP_MEMBERS;
    case 0x10:  // IS_EXECUTABLE
    case 0x11:  // EXECUTE
    case 0x12:  // IS_INSTANTIABLE
    case 0x13:  // INSTANTIATE
        return CAP_INTEROP_EXECUTE;
    case 0x20:  // HAS_ARRAY_ELEMENTS
    case 0x21:  // READ_ARRAY_ELEMENT
    case 0x22:  // WRITE_ARRAY_ELEMENT
    case 0x23:  // ARRAY_SIZE
        return CAP_INTEROP_ARRAY;
    case 0x30:  // IS_BOOLEAN
    case 0x31:  // AS_BOOLEAN
    case 0x32:  // IS_NUMBER
    case 0x33:  // AS_NUMBER
    case 0x34:  // IS_STRING
    case 0x35:  // AS_STRING
    case 0x36:  // IS_NULL
        return CAP_INTEROP_PRIMITIVES;
    case 0x40:  // IS_POINTER
    case 0x41:  // AS_POINTER
        return CAP_INTEROP_POINTER;
    case 0x50:  // IS_EXCEPTION
    case 0x51:  // THROW
    case 0x52:  // GET_EXCEPTION_TYPE
        return CAP_INTEROP_EXCEPTION;
    default:
        return static_cast<InteropCapability>(0);  // unknown -> unsupported
    }
}

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

}  // namespace vortex::runtime::interop
