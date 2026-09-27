// Stage 16: IC specialization (mono/bic feedback becomes
// klass-guarded fast paths; Rule 30 license lives in the profiles).
// Split from src/j2/passes.cpp — one pass per file.
#include "vortex/j2/passes.hpp"

#include <algorithm>
#include <cstring>


#include "passes_internal.hpp"
#include "vortex/runtime/object_model.hpp"

namespace vortex::j2 {
using ir::AccessKind;
using Graph = ir::Graph;
using ir::CallShape;
using ir::CondCode;
using ir::GuardKind;
using ir::JType;
using ir::kNoNode;
using ir::Node;
using ir::NodeId;
using ir::NodeKind;

// ---- stage 16: IC specialization --------------------------------------------------------

/// Resolves a T0 IC klass id to the Klass* the guard compares against
/// (same authoritative id() lookup as the J1 strengthener).
const void* klass_addr_by_id(const std::vector<void*>& klass_addrs,
                             uint32_t klass_id) {
    for (const void* addr : klass_addrs) {
        if (addr != nullptr &&
            static_cast<const Klass*>(addr)->id() == klass_id) {
            return addr;
        }
    }
    return nullptr;
}

/// Stage 16 — small IC specialization: a Monomorphic field site becomes a
/// ClassGuard (profiled klass) + raw offset access; the generic helper path
/// remains as the deopt target (Rule 34: specialization keeps a fallback).
/// Effect position is preserved exactly (no reordering).
uint32_t specialize_ic(ir::Graph& g, BuiltGraph& built) {
    if (built.profiles == nullptr || built.ics == nullptr ||
        built.klass_addrs == nullptr) {
        return 0;
    }
    uint32_t specialized = 0;
    // Snapshot the walk range first: adds inside the loop reallocate the
    // node store (any Node& held across an add would dangle) and grow the
    // count. New nodes are never Field accesses, so they need no visit.
    const uint32_t walk_count = g.node_count();
    for (uint32_t id = 0; id < walk_count; ++id) {
        const Node& node = g.node(id);
        if (node.dead) continue;
        const bool is_load = node.kind == NodeKind::Load;
        const bool is_store = node.kind == NodeKind::Store;
        if (!is_load && !is_store) continue;
        if (static_cast<AccessKind>(node.aux) != AccessKind::Field) continue;
        const uint32_t insn = built.insn_of[id];
        if (insn >= built.ics->size()) continue;
        const ugb::IcSlot& ic = (*built.ics)[insn];
        if (ic.state != ugb::IcState::Monomorphic) continue;
        const void* klass = klass_addr_by_id(*built.klass_addrs,
                                             ic.mono.klass_or_shape_id);
        if (klass == nullptr) continue;
        // The access node's LAST data input is its FrameState (the builder
        // attaches it so synthetic guards satisfy Rule 31).
        if (node.data_inputs.size() < 2) continue;
        const NodeId fs = node.data_inputs.back();
        const NodeId obj = node.data_inputs[0];
        const NodeId control = node.control;
        const NodeId effect_in = node.effect_in;
        const NodeId value = is_store ? node.data_inputs[1] : kNoNode;
        const uint32_t node_block = built.block_of[id];
        // Everything below adds nodes: `node` is stale from here — use the
        // captured ids only (Rule 48: indices, never pointers).
        const NodeId guard = g.add_aux(NodeKind::ClassGuard, {obj, fs},
                                       ic.mono.klass_or_shape_id,
                                       built.blocks[node_block].region);
        sync_built(g, built);
        built.block_of[guard] = node_block;
        built.types[guard] = JType::Ref;
        built.insn_of[guard] = insn;
        // The synthesized guard needs its own deopt record: register it in
        // the guard list the deopt-record builder walks (Rule 30 — every
        // speculative node carries its state-exact resume).
        built.guards.push_back(guard);
        // Raw access at the profiled slot (byte offset header + slot * 8).
        const int64_t byte_off =
            static_cast<int64_t>(sizeof(ObjectHeader)) +
            kTaggedSlotBytes * static_cast<int64_t>(ic.mono.target);
        if (is_load) {
            const NodeId raw = g.add_aux(
                NodeKind::Load, {guard},
                static_cast<uint32_t>(AccessKind::RawOffset),
                control, effect_in);
            sync_built(g, built);
            g.node(raw).const_value = byte_off;
            built.block_of[raw] = node_block;
            built.types[raw] = built.types[id];
            built.insn_of[raw] = insn;
            replace_data_uses(g, id, raw);
            replace_effect_input(g, id, raw);
        } else {
            const NodeId raw = g.add_aux(
                NodeKind::Store, {guard, value},
                static_cast<uint32_t>(AccessKind::RawOffset),
                control, effect_in);
            sync_built(g, built);
            g.node(raw).const_value = byte_off;
            built.block_of[raw] = node_block;
            built.insn_of[raw] = insn;
            replace_effect_input(g, id, raw);
        }
        kill(g, id);
        ++specialized;
    }
    return specialized;
}

}  // namespace vortex::j2
