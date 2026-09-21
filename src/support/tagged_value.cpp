#include "vortex/support/tagged_value.hpp"

namespace vortex {

std::string TaggedValue::to_string() const {
    if (is_smi()) return std::to_string(as_smi());
    if (is_null()) return "null";
    if (is_undefined()) return "undefined";
    if (is_boolean()) return as_boolean_unchecked() ? "true" : "false";
    if (is_heap_object()) return "<object>";
    return "<tagged 0x" + std::to_string(bits_) + ">";
}

}  // namespace vortex
