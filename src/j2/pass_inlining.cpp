// Stages 14+15: bounded inlining — direct calls, single-block
// callees, depth/site/node caps, chained deopt frames (Rule 39/113).
// Split from src/j2/passes.cpp — one pass per file.
#include "vortex/j2/passes.hpp"

#include <algorithm>
#include <cstring>


#include "passes_internal.hpp"
#include "vortex/j2/fast_jit.hpp"
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

// ---- stages 14+15: bounded inlining ------------------------------------------------------

uint32_t param_index_of(const ir::Graph& cg, NodeId param_id) {
    uint32_t idx = 0;
    for (const Node& n : cg.nodes()) {
        if (n.id == param_id) return idx;
        if (n.kind == NodeKind::Parameter) ++idx;
    }
    return idx;
}

bool builds_single_block(
    const ugb::UGBModule& module, const ugb::UGBMethod& callee,
    size_t node_cap, const BuiltGraph& parent,
    std::unique_ptr<BuiltGraph>& out) {
    // The callee builds under the SAME profile/speculation material as the
    // caller (POLY_* lowering license + IC specialization consistency).
    GraphBuilderParams cb{};
    cb.module = &module;
    cb.method = &callee;
    cb.node_cap = node_cap;
    cb.interop = parent.interop;
    // Callee pc spaces index the CALLEE's tables — the caller's tables
    // would misalign every IC slot. No callee material -> build cold
    // (profiles/ics null = conservative forms).
    cb.profiles = callee.profiles.empty() ? nullptr : &callee.profiles;
    cb.ics = callee.ics.empty() ? nullptr : &callee.ics;
    cb.klass_addrs = parent.klass_addrs;
    BuildResult cr = build_graph(cb);
    if (!cr.ok) return false;
    if (cr.out->blocks.size() != 1) return false;
    out = std::move(cr.out);
    return true;
}

uint32_t inline_calls(ir::Graph& g, BuiltGraph& built,
                      const PipelineBudget& budget,
                      const ugb::UGBModule& module) {
    // Bytecode pc AFTER the instruction at `pc` in method `method_id` (the
    // call-return continuation for the deopt record, Rule 113).
    // decode_at advances its offset argument BY REFERENCE past the decoded
    // instruction (ugb::InstructionStream contract), so returning `cur`
    // after the decode IS the next pc.
    const auto next_pc_after = [&module](uint32_t method_id,
                                         uint32_t pc) -> uint32_t {
        if (method_id >= module.method_table.size()) return pc;
        const ugb::UGBMethod& m = module.method_table[method_id];
        ugb::InstructionStream stream(m.code.data(), m.code.size());
        size_t cur = pc;
        ugb::Instruction ins;
        if (!stream.decode_at(cur, ins)) return pc;
        return static_cast<uint32_t>(cur);
    };
    uint32_t inlined = 0;
    // Termination invariants (docs/tier-j4.md section 12.2): the inline
    // chain guard is ALWAYS on — a method may never appear twice on one
    // splice chain and the chain may never exceed the deopt-frame ABI
    // capacity, so the sweep terminates deterministically for ACYCLIC and
    // cyclic call graphs alike (tier-j4.md section 6: explosion is avoided
    // with call-graph cycle detection, never with budgets). Note the
    // behavior caveat vs the M2/M3 pipelines: on CYCLIC call graphs the
    // chain guard can refuse an inline the old caps would have spliced
    // (inlining is optional — semantics are unaffected, Rule 18; the
    // depth-per-sweep caps still bound J2/J3 shapes identically on acyclic
    // graphs).
    for (uint32_t depth = 0; depth < budget.inline_depth_cap; ++depth) {
        bool any = false;
        for (uint32_t call_id = 0; call_id < g.node_count(); ++call_id) {
            // Snapshot the call's fields BEFORE any add: the splice below
            // reallocates the node store, and a Node& held across an add
            // would dangle (ASan-verified; Rule 48 indices only).
            const Node& call0 = g.node(call_id);
            if (call0.dead || call0.kind != NodeKind::Call) continue;
            const uint32_t shape = (call0.aux >> kShapeShift) & 0x3;
            if (shape != static_cast<uint32_t>(CallShape::Direct)) continue;
            if (call0.data_inputs.size() < 2) continue;
            const std::vector<NodeId> call_args(call0.data_inputs.begin(),
                                                call0.data_inputs.end());
            const NodeId call_effect_in = call0.effect_in;
            const int64_t call_const_value = call0.const_value;
            const uint32_t call_insn = built.insn_of[call_id];
            const uint32_t callee_token = call0.aux & kAuxMask;
            if (callee_token >= module.runtime.method_resolution.size()) {
                continue;
            }
            const int64_t resolved =
                module.runtime.method_resolution[callee_token];
            if (resolved < 0 ||
                static_cast<size_t>(resolved) >= module.method_table.size()) {
                continue;
            }
            const ugb::UGBMethod& callee =
                module.method_table[static_cast<size_t>(resolved)];
            if (callee.id == built.method_id) continue;  // no recursion
            const std::vector<uint32_t>& chain =
                call_id < built.inline_paths.size()
                    ? built.inline_paths[call_id]
                    : std::vector<uint32_t>{};
            // Call-graph cycle guard (Rule 16): a method may never appear
            // twice on one splice chain — chain lengths are bounded by the
            // module's method count, so the sweep terminates.
            bool on_chain = false;
            for (const uint32_t m : chain) {
                if (m == callee.id) {
                    on_chain = true;
                    break;
                }
            }
            if (on_chain) continue;
            // Deopt-frame ABI capacity (the M4 review's blocker): a guard
            // inside the spliced callee produces one deopt frame per link
            // of the chain (root + every spliced method). The record
            // builder's kMaxDeoptFrames capacity must never be exceeded —
            // silently truncating outer frames would resume state-exact
            // WRONG (Rules 39/42/113). Splicing callee C into a chain of
            // length L yields L+1 frames, so L must stay <= kMaxDeoptFrames
            // - 1. This is the REAL enforcement of the J4 inline bound
            // (j4_budget's inline_depth_cap names the same number); it
            // holds in every mode because the record capacity does.
            if (chain.size() + 1 > j2::kMaxDeoptFrames) continue;
            std::unique_ptr<BuiltGraph> callee_built;
            if (!builds_single_block(module, callee,
                                     budget.inline_callee_node_cap, built,
                                     callee_built)) {
                if (std::getenv("VORTEX_J2_TRACE")) {
                    fprintf(stderr, "[j2-inline] skip single-block: %s\n",
                            callee.name.c_str());
                }
                continue;
            }
            const ir::Graph& cg = *callee_built->graph;
            if (g.node_count() + cg.node_count() > budget.node_cap) {
                continue;  // graceful: keep the call, stop growing
            }
            // Arity check: call inputs = [args..., FrameState].
            const size_t argc = call_args.size() - 1;
            uint32_t param_count = 0;
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                if (cg.node(k).kind == NodeKind::Parameter) ++param_count;
            }
            if (param_count < argc) {
                // Malformed: the callee cannot name its own arguments.
                if (std::getenv("VORTEX_J2_TRACE")) {
                    fprintf(stderr,
                            "[j2-inline] skip arity: %s params=%u argc=%zu\n",
                            callee.name.c_str(), param_count, argc);
                }
                continue;
            }
            // params > argc are the callee's register-file entries beyond
            // the argument window (the entry ABI materializes every
            // register; T0 initializes the tail to undefined). They splice
            // as undefined constants, never as caller values.
            if (built.inlined_sites >= budget.inline_site_cap) break;

            // ---- splice ----------------------------------------------------
            const uint32_t base = g.node_count();
            const uint32_t call_block = built.block_of[call_id];
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                std::vector<NodeId> inputs;
                inputs.reserve(src.data_inputs.size());
                for (const NodeId in : src.data_inputs) {
                    inputs.push_back(static_cast<NodeId>(base + in));
                }
                const NodeId ctrl =
                    src.control != kNoNode
                        ? static_cast<NodeId>(base + src.control)
                        : kNoNode;
                const NodeId eff =
                    src.effect_in != kNoNode
                        ? static_cast<NodeId>(base + src.effect_in)
                        : kNoNode;
                g.add_vec(src.kind, inputs, ctrl, eff);
            }
            sync_built(g, built);
            // Origin stamp: every spliced node is marked so later passes
            // never treat id order as program order across the splice line
            // (optimize_guards; any future earlier-id-means-earlier pass
            // must consult this too).
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                built.spliced[base + k] = 1;
            }
            // Map + fix payload words. cn is re-fetched after any add
            // (clone_const may reallocate the node store).
            for (const Node& src : cg.nodes()) {
                const NodeId copy = static_cast<NodeId>(base + src.id);
                {
                    Node& cn = g.node(copy);
                    cn.aux = src.aux;
                    cn.const_value = src.const_value;
                }
                built.types[copy] =
                    src.id < callee_built->types.size()
                        ? callee_built->types[src.id]
                        : JType::Unknown;
                built.block_of[copy] = call_block;
                built.insn_of[copy] = call_insn;
                if (src.kind == NodeKind::Parameter) {
                    // vreg order == parameter creation order.
                    const uint32_t pidx = param_index_of(cg, src.id);
                    NodeId arg = kNoNode;
                    if (pidx < argc) {
                        arg = call_args[pidx];
                    } else {
                        // Beyond the argument window: the callee's entry
                        // reads undefined (the J2 entry prologue writes the
                        // same pattern for registers past argc).
                        arg = clone_const(g, built, call_id,
                                          TaggedValue::undefined().raw(),
                                          JType::Unknown);
                    }
                    if (arg != kNoNode) {
                        replace_uses(g, copy, arg);
                        g.node(copy).dead = true;
                    }
                }
                if (src.kind == NodeKind::Start) {
                    g.node(copy).control = kNoNode;
                    g.node(copy).dead = true;  // control flows from call block
                }
            }
            // The callee's guards keep their deopt semantics in the caller:
            // register every spliced guard so the deopt-record builder sees
            // it (Rule 30 — a guard without a record cannot resume).
            for (const NodeId gg : callee_built->guards) {
                built.guards.push_back(static_cast<NodeId>(base + gg));
            }
            // Region control -> the call's block region; effect entry ->
            // the call's effect input.
            const Node& callee_start = cg.nodes()[0];
            (void)callee_start;
            // The callee is single-block: its Start's region child is the
            // body region (id from the callee layout) — relink to the call
            // block's region.
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                if (src.kind == NodeKind::Region && src.control != kNoNode &&
                    cg.node(src.control).kind == NodeKind::Start) {
                    g.node(base + src.id).control =
                        built.blocks[call_block].region;
                }
            }
            // Effect chain entry: the builder gives the callee's FIRST
            // effect node effect_in = kNoNode (nothing precedes it inside
            // the callee). Wire it to the call's effect_in so the spliced
            // body sits on the caller's chain — without this, the caller's
            // prior stores/loads are unordered against the spliced ones
            // (Rule 55: side-effect order is part of the pass contract).
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                if (ir::is_effectful(src.kind) && src.effect_in == kNoNode) {
                    g.node(base + src.id).effect_in = call_effect_in;
                    break;  // single-block callee: exactly one chain entry
                }
            }
            // Find the call's effect consumers and splice them to the
            // callee's last live effect.
            NodeId callee_last_effect = kNoNode;
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                if (ir::is_effectful(src.kind)) {
                    callee_last_effect = static_cast<NodeId>(base + src.id);
                }
            }
            if (callee_last_effect != kNoNode) {
                replace_effect_input(g, call_id, callee_last_effect);
            } else {
                replace_effect_input(g, call_id, call_effect_in);
            }
            // Return value -> the call's data users.
            NodeId result = kNoNode;
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                if (src.kind == NodeKind::Return) {
                    result = static_cast<NodeId>(base + src.data_inputs[0]);
                }
            }
            if (result == kNoNode) {
                // Unit return: substitute the null word.
                result = g.add_const(static_cast<int64_t>(0x3));
                built.types.resize(g.node_count(), JType::Unknown);
                built.types[result] = JType::Null;
                built.block_of.resize(g.node_count(), UINT32_MAX);
                built.block_of[result] = call_block;
            }
            replace_data_uses(g, call_id, result);
            // Guard/call/frame nodes inside the callee keep their deopt
            // FrameState (last data input, remapped); record the CALLER
            // continuation so the deopt generator emits the frame chain
            // (Rule 113). FrameStates are mapped so a depth-2 guard walks
            // guard.fs -> middle fs -> root fs through the same table.
            const NodeId caller_fs = call_args.back();
            const uint32_t caller_method =
                caller_fs != kNoNode
                    ? static_cast<uint32_t>(
                          std::max<int64_t>(g.node(caller_fs).const_value, 0))
                    : built.method_id;
            const uint32_t return_pc =
                caller_fs != kNoNode
                    ? next_pc_after(caller_method, g.node(caller_fs).aux)
                    : 0;
            const BuiltGraph::InlineCallerInfo continuation{
                caller_fs,
                static_cast<uint32_t>(call_const_value), return_pc};
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                const Node& src = cg.node(k);
                if (ir::is_guard(src.kind) || ir::is_effectful(src.kind) ||
                    src.kind == NodeKind::FrameState) {
                    built.inline_caller_frame[base + src.id] = continuation;
                }
            }
            kill(g, call_id);
            ++built.inlined_sites;
            ++inlined;
            any = true;
            // Extend the inline chain: every spliced CALL node carries the
            // caller chain + the callee id, so the cycle guard above sees
            // the full path that produced it (sync_built already resized
            // the table for the spliced nodes).
            for (uint32_t k = 0; k < cg.node_count(); ++k) {
                if (cg.node(k).kind != NodeKind::Call) continue;
                std::vector<uint32_t>& next = built.inline_paths[base + k];
                if (next.empty()) {
                    next.reserve(chain.size() + 2);
                    next.push_back(built.method_id);
                    for (const uint32_t m : chain) {
                        if (m != built.method_id) next.push_back(m);
                    }
                }
                next.push_back(callee.id);
            }
        }
        if (!any) break;
    }
    return inlined;
}

}  // namespace vortex::j2
