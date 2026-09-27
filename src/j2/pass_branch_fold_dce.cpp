// Stages 5+6: branch folding, reachability sweep, dead-code
// elimination (one pass — the CFG rewrite is atomic with the DCE).
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

// ---- stage 5+6: branch folding + reachability + DCE --------------------------------

uint32_t fold_branches_and_sweep(ir::Graph& g, BuiltGraph& built) {
    uint32_t changed = 0;
    for (uint32_t b = 0; b < built.blocks.size(); ++b) {
        Block& blk = built.blocks[b];
        if (blk.control_end == kNoNode) continue;
        Node& end = g.node(blk.control_end);
        if (end.dead || end.kind != NodeKind::If) continue;
        if (end.data_inputs.empty()) continue;
        const Node& cond = g.node(end.data_inputs[0]);
        if (cond.kind != NodeKind::Const) continue;
        if (blk.succs.size() < 2) continue;
        // Truthiness on the canonical tagged word (T0 TaggedValue::truthy,
        // Rule 18): even words are Smis (truthy iff the word is nonzero);
        // odd words are the special/heap encodings — false (0xB), null
        // (0x3) and undefined (0x7) are falsey, true (0xF) is truthy, and
        // the builder never emits heap-pointer constants.
        const uint64_t w = static_cast<uint64_t>(cond.const_value);
        const bool truthy = (w & 1) == 0 ? (w != 0) : (w == 0xF);
        const bool take_target = truthy == blk.target_is_true_edge;
        const uint32_t keep = take_target ? blk.succs[0] : blk.succs[1];
        const uint32_t drop = take_target ? blk.succs[1] : blk.succs[0];
        end.kind = NodeKind::End;
        end.data_inputs.clear();
        auto& succs = blk.succs;
        succs.erase(std::remove(succs.begin(), succs.end(), drop),
                    succs.end());
        auto& preds_keep = built.blocks[keep].preds;
        if (std::find(preds_keep.begin(), preds_keep.end(), b) ==
            preds_keep.end()) {
            preds_keep.push_back(b);
        }
        auto& preds_drop = built.blocks[drop].preds;
        preds_drop.erase(std::remove(preds_drop.begin(), preds_drop.end(), b),
                         preds_drop.end());
        kill(g, end.id);
        ++changed;
    }
    // Unreachable-block sweep.
    std::vector<bool> reach(built.blocks.size(), false);
    std::vector<uint32_t> work{built.entry_block};
    reach[built.entry_block] = true;
    while (!work.empty()) {
        const uint32_t b = work.back();
        work.pop_back();
        for (const uint32_t s : built.blocks[b].succs) {
            if (!reach[s]) {
                reach[s] = true;
                work.push_back(s);
            }
        }
    }
    for (uint32_t b = 0; b < built.blocks.size(); ++b) {
        if (!reach[b]) {
            changed += kill_block_nodes(g, built, b);
            built.blocks[b].bytecode_end =
                built.blocks[b].bytecode_begin;  // diagnostics only
            ++changed;
        }
    }
    return changed;
}

}  // namespace vortex::j2
