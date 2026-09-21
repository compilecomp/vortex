// vortex — the ultimate JIT, in C++26.
//
// Public umbrella header: the complete API surface. Individual subsystem
// headers are preferred for internal use.
#pragma once

// support
#include "vortex/support/arena.hpp"
#include "vortex/support/hash.hpp"
#include "vortex/support/log.hpp"
#include "vortex/support/result.hpp"
#include "vortex/support/tier.hpp"
#include "vortex/support/tagged_value.hpp"

// runtime
#include "vortex/runtime/object_model.hpp"
#include "vortex/runtime/tiering.hpp"

// UGB
#include "vortex/ugb/assembler.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/disassembler.hpp"
#include "vortex/ugb/module.hpp"
#include "vortex/ugb/opcode.hpp"
#include "vortex/ugb/verifier.hpp"

// T0 interpreter
#include "vortex/vm/interpreter.hpp"

// reference frontend (an example language port — see docs/porting.md)
#include "vortex/frontends/reference/emitter.hpp"
#include "vortex/frontends/reference/lexer.hpp"
#include "vortex/frontends/reference/parser.hpp"

// codegen
#include "vortex/codegen/codegen.hpp"
#include "vortex/codegen/x64/assembler.hpp"

// IR
#include "vortex/ir/ciog.hpp"
#include "vortex/ir/son_graph.hpp"

// JIT tiers
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j1/stencil.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/j3/full_jit.hpp"
#include "vortex/j4/max_jit.hpp"

// deopt + GC
#include "vortex/deopt/rbpd.hpp"
#include "vortex/gc/icggc.hpp"

// infrastructure systems
#include "vortex/infra/code_cache.hpp"
#include "vortex/infra/cpu_dispatch.hpp"
#include "vortex/infra/dependency.hpp"
#include "vortex/infra/ffi.hpp"
#include "vortex/infra/observability.hpp"
#include "vortex/infra/power.hpp"
#include "vortex/infra/security.hpp"
#include "vortex/infra/snapshot.hpp"
#include "vortex/infra/threading.hpp"

namespace vortex {

/// Library version of the Vortex JIT stack.
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;

}  // namespace vortex
