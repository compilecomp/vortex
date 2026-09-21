// Mini reference frontend — UGB emitter.
//
// Lowers the Mini AST into a verified-capable UGBModule. This file is the
// executable form of docs/porting.md Step 1: language semantics -> UGB
// instructions, with canonical opcodes and no speculation (speculation is the
// engine's job, driven by profiles).
#pragma once

#include <string_view>

#include "vortex/frontends/reference/parser.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::frontends::reference {

using support::Result;

/// Compiles Mini source to a UGB module. The module declares the capability
/// set Mini relies on (`core`, `typed_arithmetic`, `closures`-free subset:
/// control flow + calls). On success the module is ready for
/// ugb::verify_module + vm::Interpreter::run.
Result<ugb::UGBModule> compile_mini(std::string_view source);

}  // namespace vortex::frontends::reference
