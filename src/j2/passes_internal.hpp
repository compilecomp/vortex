// Internal cross-TU declarations of the J2 pass pipeline. Shared
// graph-mutation helpers are defined in src/j2/passes.cpp; every named
// pass lives in its own translation unit (src/j2/pass_*.cpp); the J3
// pipeline reuses them so both tiers share one graph-mutation discipline
// (Rule 19: one IR, one set of invariants).
#pragma once

#include <cstdint>
#include <vector>

#include "vortex/j2/passes.hpp"
#include "vortex/j2/graph_builder.hpp"
#include "vortex/ir/son_graph.hpp"
#include "vortex/ir/value_types.hpp"

namespace vortex::j2 {

/// Grows the BuiltGraph side tables to match the node store.
void sync_built(const ir::Graph& g, BuiltGraph& built);

/// Replaces every use (data, control, effect) of `from` with `to`.
uint32_t replace_uses(ir::Graph& g, ir::NodeId from, ir::NodeId to);

/// Replaces only DATA uses (result consumers), leaving effect chains alone.
uint32_t replace_data_uses(ir::Graph& g, ir::NodeId from, ir::NodeId to);

/// Redirects the effect chain: consumers of `from`'s effect now consume `to`.
uint32_t replace_effect_input(ir::Graph& g, ir::NodeId from, ir::NodeId to);

void kill(ir::Graph& g, ir::NodeId id);

/// Kills every live node in one block (branch folding uses it).
uint32_t kill_block_nodes(ir::Graph& g, const BuiltGraph& built,
                          uint32_t block);

/// RPO + immediate dominators over the BuiltGraph CFG (block indices).
void compute_dominators(const BuiltGraph& built, std::vector<uint32_t>& rpo,
                        std::vector<uint32_t>& idom);

/// The Smi domain bounds (the int63 payload range, T0-verbatim).
int64_t smi_min();
int64_t smi_max();

/// Clones a constant node beside `near_id` and types it (canonicalization,
/// folding and inlining share it).
ir::NodeId clone_const(ir::Graph& g, BuiltGraph& built, ir::NodeId near_id,
                       int64_t value, ir::JType t);

// ---- the named passes (one TU each, src/j2/pass_*.cpp) ---------------------

/// Stage 2: canonicalization.
uint32_t canonicalize(ir::Graph& g, BuiltGraph& built);

/// Stages 3+4: constant folding.
uint32_t fold_constants(ir::Graph& g, BuiltGraph& built);

/// Stages 5+6: branch folding + reachability + DCE.
uint32_t fold_branches_and_sweep(ir::Graph& g, BuiltGraph& built);

/// Stages 7+8: scoped value numbering (needs RPO + idom).
uint32_t value_number(ir::Graph& g, BuiltGraph& built,
                      const std::vector<uint32_t>& rpo,
                      const std::vector<uint32_t>& idom);

/// Stages 10/12/13: guard optimization.
uint32_t optimize_guards(ir::Graph& g, BuiltGraph& built);

/// Stage 16: IC specialization.
uint32_t specialize_ic(ir::Graph& g, BuiltGraph& built);

/// Stages 14+15: bounded inlining.
uint32_t inline_calls(ir::Graph& g, BuiltGraph& built,
                      const PipelineBudget& budget,
                      const ugb::UGBModule& module);

// ---- small shared predicates/constants -------------------------------------

inline bool is_pure_kind(ir::NodeKind k) { return ir::is_pure(k); }
inline bool is_guard_kind(ir::NodeKind k) { return ir::is_guard(k); }
inline constexpr uint32_t kAuxMask = 0xFFFF;
inline constexpr uint32_t kShapeShift = 16;

}  // namespace vortex::j2
