// J2 — light Sea-of-Nodes graph construction (docs/tier-j2.md, docs/roadmap.md
// M2). Builds the shared optimizer IR (vortex::ir, Rule 19) from UGB bytecode
// plus T0 profiles: register values are tagged SSA nodes, typed arithmetic is
// lowered into SmiGuard -> Untag -> raw op -> OverflowGuard -> Tag with
// FrameState snapshots on every guard (Rule 30/31/42), and effectful nodes
// chain in program order (no reordering before the pass pipeline proves it).
//
// Structural laws implemented here:
//   Rule 13 — J2 builds a LIGHT graph: one Region per UGB basic block,
//             Phis at dominance frontiers only, no CIOG expansion.
//   Rule 30 — every profile-driven specialization emits a guard + FrameState.
//   Rule 42 — every guard references a complete FrameState (all vregs).
//   Rule 56 — construction is deterministic: fixed decode order, fixed
//             block order, no hash iteration in emission decisions.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "vortex/ir/son_graph.hpp"
#include "vortex/ir/value_types.hpp"
#include "vortex/support/arena.hpp"
#include "vortex/support/result.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::j2 {

/// One basic block of the built graph. Control flows Region -> (If -> Region
/// | Return | End); data nodes remember their block through block_of[].
struct Block {
    ir::NodeId region = ir::kNoNode;   // the Region (or Loop) control node
    ir::NodeId control_end = ir::kNoNode;  // If/Return/End that ends it
    ir::NodeId effect_in = ir::kNoNode;    // effect-chain head on entry
    std::vector<uint32_t> preds;
    std::vector<uint32_t> succs;
    uint32_t bytecode_begin = 0;  // first bytecode offset (diagnostics)
    uint32_t bytecode_end = 0;
    /// Branch probability of the block-ending conditional's TRUE edge, in
    /// thousandths (permille). 500 = unknown/split (Rule 22: profile-driven).
    uint32_t true_prob_permille = 500;
    /// Which successor (in `succs`) is the condition-TRUE edge: true = the
    /// branch TARGET, false = the target is taken on the FALSE edge
    /// (JUMP_FALSE). Layout consumes this; never guess (Rule 56).
    bool target_is_true_edge = true;
};

/// Builder output: the graph plus the metadata the pipeline and the backend
/// consume. All vectors are indexed by NodeId.
struct BuiltGraph {
    /// Ownership: the arena backs the graph's bulk storage; both live as
    /// long as the BuiltGraph does (unique ownership — Rule 69's explicit-
    /// ownership clause; the graph itself is compiled once per J2 job).
    std::unique_ptr<support::Arena> arena;
    std::unique_ptr<ir::Graph> graph;
    std::vector<ir::JType> types;       // per node
    std::vector<uint32_t> block_of;     // per node -> block index
    std::vector<Block> blocks;          // bytecode order
    /// 1 = the node came from an inlined callee splice. Id order is program
    /// order only WITHIN one origin; guard merges and any other
    /// earlier-id-means-earlier reasoning must not cross this line.
    std::vector<uint8_t> spliced;       // per node
    uint32_t entry_block = 0;
    /// OSR-capable blocks: bytecode offsets with incoming backedges (the
    /// same set J1 uses for its OSR stubs).
    std::vector<uint32_t> osr_block_offsets;
    /// vreg count of the compiled method (frame layout input).
    uint32_t register_count = 0;
    /// Per-block ENTRY register maps (the merge/Phi result the block's
    /// body starts with) — OSR stubs materialize register files through
    /// these (the OSR ABI writes whole T0 register files).
    std::vector<std::vector<ir::NodeId>> block_entry_values;
    /// Guard nodes in program order, each with its FrameState reference
    /// (data input index inside the guard's data_inputs) — the deopt-record
    /// generator walks this list.
    std::vector<ir::NodeId> guards;
    /// Block order (reverse post-order) and immediate dominators from
    /// construction; the pipeline recomputes them after CFG rewrites.
    std::vector<uint32_t> rpo;
    std::vector<uint32_t> idom;
    /// Originating instruction index per node (UINT32_MAX for synthetic
    /// nodes) — profile-driven passes key off this (Rule 8 site identity).
    std::vector<uint32_t> insn_of;
    /// Pipeline inputs (stages 15/16 consume profiles, ICs and the klass
    /// table; null = no speculation material, passes stay conservative).
    const ugb::UGBModule* module = nullptr;
    const std::vector<ugb::ProfileSlot>* profiles = nullptr;
    const std::vector<ugb::IcSlot>* ics = nullptr;
    const std::vector<void*>* klass_addrs = nullptr;
    /// Inline metadata: spliced guard/call/frame node -> the continuation
    /// the deopt generator chains behind the callee frame (Rule 113). The
    /// entry describes HOW to resume the caller: its FrameState (the vregs
    /// the caller held at the call), the call's dst vreg (where the callee
    /// result is injected) and the bytecode pc AFTER the call.
    struct InlineCallerInfo {
        ir::NodeId frame_state = ir::kNoNode;  // caller's FrameState node
        uint32_t inject_dst = 0xFFFFFFFFu;     // call dst vreg
        uint32_t return_pc = 0;                // pc after the call
    };
    std::unordered_map<uint32_t, InlineCallerInfo> inline_caller_frame;
    uint32_t inlined_sites = 0;
    uint32_t argc = 0;
    uint32_t method_id = 0;
};

/// Builder diagnostics: why construction refused the method (Rule 76 — no
/// silent fallbacks). The caller keeps the method on J1/T0.
enum class BuildError : uint32_t {
    None = 0,
    UnsupportedOpcode,
    BudgetExceeded,   // node cap hit (graceful: stay on J1, retry at J3)
    MalformedBranch,  // branch target not on an instruction boundary
    DecodeError,
};


/// Parameters: profiles/ICs are the T0-maintained per-instruction tables
/// (may be null when the method never ran — the builder then takes no
/// speculation and emits canonical forms with guards).
struct GraphBuilderParams {
    const ugb::UGBModule* module = nullptr;
    const ugb::UGBMethod* method = nullptr;
    const std::vector<ugb::ProfileSlot>* profiles = nullptr;
    /// IC slots + resolved klass addresses (stage 16 consumes them; null =
    /// no speculation material, passes stay conservative).
    const std::vector<ugb::IcSlot>* ics = nullptr;
    const std::vector<void*>* klass_addrs = nullptr;
    /// Node cap (Budget::node_cap). Construction refuses (BudgetExceeded)
    /// beyond it — J2 must never become a large compile.
    size_t node_cap = 20'000;
};

struct BuildResult {
    bool ok = false;
    BuildError error = BuildError::None;
    uint32_t error_pc = 0;
    std::unique_ptr<BuiltGraph> out;  // null on failure
};

/// Builds the light SoN graph for one method. Deterministic (Rule 56);
/// refusals carry a named reason (Rule 76) and the caller keeps the method
/// on its current tier (Rule 11).
BuildResult build_graph(const GraphBuilderParams& params);

const char* build_error_message(BuildError e) noexcept;

}  // namespace vortex::j2
