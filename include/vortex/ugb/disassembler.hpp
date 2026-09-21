// UGB disassembler — textual rendering of encoded modules (the mirror of the
// text assembler). Used by `vx dis` and by golden tests.
#pragma once

#include <string>

#include "vortex/ugb/module.hpp"

namespace vortex::ugb {

/// Disassembles an entire module to the assembler text format.
std::string disassemble_module(const UGBModule& module);

/// Disassembles one method body.
std::string disassemble_method(const UGBModule& module, const UGBMethod& method);

}  // namespace vortex::ugb
