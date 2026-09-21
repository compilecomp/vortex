// UGB instruction-word layout constants — the single source of truth for the
// fixed-width instruction encoding (docs/ugb.md section 8):
//
//   opcode: u16 | flags: u8 | dst: u16 | src_count: u8 | srcs: u16[] | meta: u32?
//
// CEM-26 section 2 (Magic Number Ban): every byte count in this layout is a
// named constant so the decoder, the encoder, the verifier, the text
// assembler and the T0 dispatch loop cannot drift apart. The meta-field
// offset is a FUNCTION of src_count — offset arithmetic must never be
// inlined as a literal.
//
// Cost model note (CEM-26 Cost Trinity, source level): the layout is chosen
// so the T0 fast decoder reads the fixed 6-byte header in at most two
// overlapping u64 loads on x86-64/AArch64 and the meta word is at a
// src_count-computable address, i.e. one extra LEA on the hot path.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vortex::ugb {

namespace encoding {

// ---- field widths (bytes) --------------------------------------------------

inline constexpr size_t kOpcodeBytes = 2;    // Op is u16
inline constexpr size_t kFlagsBytes = 1;     // InstrFlags byte
inline constexpr size_t kDstBytes = 2;       // dst register id, u16
inline constexpr size_t kSrcCountBytes = 1;  // source register count, u8
inline constexpr size_t kSrcSlotBytes = 2;   // each source register id, u16
inline constexpr size_t kMetaBytes = 4;      // metadata token / immediate, u32

// Fixed instruction header: opcode + flags + dst + src_count.
inline constexpr size_t kInstrHeaderBytes =
    kOpcodeBytes + kFlagsBytes + kDstBytes + kSrcCountBytes;

// ---- byte lanes (little-endian serialization) --------------------------------

/// Bits per byte — the wire format is little-endian; lane shifts in decoders
/// derive from this constant instead of bare 8/16/24 literals.
inline constexpr size_t kBitsPerByte = 8;

// ---- derived quantities -----------------------------------------------------

/// Byte offset of the meta word within an instruction with `src_count`
/// sources. This is the ONLY sanctioned way to compute the offset; the
/// label-backpatcher and the codecs must all call this.
constexpr size_t meta_offset(size_t src_count) noexcept {
    return kInstrHeaderBytes + kSrcSlotBytes * src_count;
}

/// Full encoded size of an instruction.
constexpr size_t instr_size(size_t src_count, bool has_meta) noexcept {
    return meta_offset(src_count) + (has_meta ? kMetaBytes : 0);
}

/// Sources handled in registers by the T0 fast decoder. The ISA allows more,
/// but wide instructions are rare and cold: decode_fast skips the src block
/// without materializing it and the handler falls back to error/slow paths.
inline constexpr size_t kFastDecodeMaxSrcs = 3;

/// src_count is a u8 field: this is a format bound, not a tunable.
inline constexpr size_t kMaxSrcCount = 0xFF;

// ---- layout invariants (CEM-26 section 9: explicit, asserted) ----------------

static_assert(kInstrHeaderBytes == 6,
              "UGB wire format v2 fixes the instruction header at 6 bytes; "
              "changing it is a format-version event (Rule 10)");
static_assert(meta_offset(0) == 6 && meta_offset(2) == 10,
              "meta_offset must stay 6 + 2*src_count (label backpatching and "
              "the binary codec depend on it)");

}  // namespace encoding

}  // namespace vortex::ugb
