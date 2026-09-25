// Shared optimizer value-type lattice (Rule 19: J2/J3/J4 use one IR and one
// type vocabulary; tier differences are budgets and pass sets, never
// semantics).
//
// The lattice mirrors the TaggedValue encoding (include/vortex/support/
// tagged_value.hpp): a word is either a Smi (tag bit clear, int63 payload)
// or a heap reference (tagged pointer). Doubles are heap-boxed (the heap's
// double klass), so F64 values are references with a proven klass — the
// untagged double itself is a J2-internal representation produced by
// Untag-style payload loads and consumed by boxing stores.
#pragma once

#include <cstdint>

namespace vortex::ir {

enum class JType : uint8_t {
    Unknown,  // no information (join of conflicting types / any-tag word)
    Smi,      // tagged smi word (tag bit clear)
    Ref,      // heap reference (any klass)
    BoxedF64, // Ref proven to be the heap's boxed-double klass
    Null,     // the null reference constant
    Bool,     // smi 0/1 produced by comparisons
};

/// Glb join for merges (Phi inputs / block-start meets): a phi of two
/// differently-typed values is Unknown unless they agree. Null joins Ref
/// to Ref (null is a reference); Bool joins Smi (Bool values are smis).
constexpr JType type_join(JType a, JType b) noexcept {
    if (a == b) return a;
    if (a == JType::Unknown || b == JType::Unknown) return JType::Unknown;
    if ((a == JType::Null && b == JType::Ref) ||
        (a == JType::Ref && b == JType::Null)) {
        return JType::Ref;
    }
    if ((a == JType::Bool && b == JType::Smi) ||
        (a == JType::Smi && b == JType::Bool)) {
        return JType::Smi;
    }
    return JType::Unknown;
}

constexpr const char* type_name(JType t) noexcept {
    switch (t) {
    case JType::Smi: return "Smi";
    case JType::Ref: return "Ref";
    case JType::BoxedF64: return "BoxedF64";
    case JType::Null: return "Null";
    case JType::Bool: return "Bool";
    case JType::Unknown: break;
    }
    return "Unknown";
}

}  // namespace vortex::ir
