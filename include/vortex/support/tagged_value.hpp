// Tagged 64-bit values — the universal value representation of the T0 register
// file and the guest heap (docs/tier-t0.md section 1).
//
// Encoding (LSB first):
//   bit0 == 0                     -> Smi,  int63 payload stored as (value << 1)
//   (bits & 0xF) == 0b0001        -> heap object pointer, 16-byte aligned
//   subtag = (bits >> 1) & 0x7    -> immediate specials:
//                                    1 = null (0x3)
//                                    2 = undefined (0x7)
//                                    3 = false (0xB)
//                                    4 = true  (0xF)
//
// This matches docs/tier-t0.md: tags may be explicit bytes, tagged pointers,
// NaN boxing or hardware tags; Vortex M0 ships the tagged-pointer scheme.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace vortex {

class HeapObject;

class TaggedValue {
public:
    constexpr TaggedValue() noexcept : bits_(kUndefinedBits) {}

    // ---- constructors -----------------------------------------------------
    static constexpr TaggedValue smi(int64_t v) noexcept {
        return TaggedValue(static_cast<uint64_t>(v) << 1);
    }
    static constexpr TaggedValue null() noexcept { return TaggedValue(kNullBits); }
    static constexpr TaggedValue undefined() noexcept { return TaggedValue(kUndefinedBits); }
    static constexpr TaggedValue boolean(bool b) noexcept {
        return TaggedValue(b ? kTrueBits : kFalseBits);
    }
    static TaggedValue heap_pointer(const void* p) noexcept {
        const uint64_t a = reinterpret_cast<uint64_t>(p);
        // 16-byte alignment is a heap invariant (see gc::Heap).
        return TaggedValue((a & ~uint64_t{0xF}) | 0x1);
    }

    // ---- predicates -------------------------------------------------------
    constexpr bool is_smi() const noexcept { return (bits_ & 1) == 0; }
    constexpr bool is_heap_object() const noexcept {
        return (bits_ & 0xF) == 0b0001;
    }
    constexpr bool is_null() const noexcept { return bits_ == kNullBits; }
    constexpr bool is_undefined() const noexcept { return bits_ == kUndefinedBits; }
    constexpr bool is_boolean() const noexcept {
        return bits_ == kTrueBits || bits_ == kFalseBits;
    }
    constexpr bool is_pointer_like() const noexcept {
        return is_heap_object() || is_null();
    }

    // ---- accessors --------------------------------------------------------
    constexpr int64_t as_smi() const noexcept {
        return static_cast<int64_t>(bits_) >> 1;
    }
    // Smi payload bounds (int63 arithmetic, value stored shifted left by 1).
    static constexpr int64_t smi_min() noexcept {
        return -(static_cast<int64_t>(1) << 62);
    }
    static constexpr int64_t smi_max() noexcept {
        return (static_cast<int64_t>(1) << 62) - 1;
    }
    HeapObject* as_heap_object() const noexcept {
        return reinterpret_cast<HeapObject*>(bits_ & ~uint64_t{0xF});
    }
    constexpr bool as_boolean_unchecked() const noexcept { return bits_ == kTrueBits; }

    constexpr uint64_t raw() const noexcept { return bits_; }
    static constexpr TaggedValue from_raw(uint64_t bits) noexcept {
        return TaggedValue(bits);
    }

    // Truthiness used by JumpTrue/JumpFalse and generic branch lowering.
    constexpr bool truthy() const noexcept {
        if (is_smi()) return as_smi() != 0;
        if (bits_ == kFalseBits || bits_ == kNullBits || bits_ == kUndefinedBits) {
            return false;
        }
        return true;  // heap objects are truthy
    }

    // Equality used by Eq.Ref / Eq.Null (identity semantics for references).
    constexpr bool reference_equals(TaggedValue other) const noexcept {
        return bits_ == other.bits_;
    }

    std::string to_string() const;

    constexpr bool operator==(const TaggedValue&) const = default;

private:
    explicit constexpr TaggedValue(uint64_t bits) noexcept : bits_(bits) {}

    static constexpr uint64_t kNullBits = 0x3;
    static constexpr uint64_t kUndefinedBits = 0x7;
    static constexpr uint64_t kFalseBits = 0xB;
    static constexpr uint64_t kTrueBits = 0xF;

    uint64_t bits_;
};

static_assert(sizeof(TaggedValue) == 8, "TaggedValue must stay a single word");

}  // namespace vortex
