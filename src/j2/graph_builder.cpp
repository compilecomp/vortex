// J2 light Sea-of-Nodes construction from UGB + T0 profiles
// (docs/tier-j2.md, docs/ir-son-ciog.md section 3.1). See the header for the
// law inventory. Implementation shape:
//
//   Phase A (scan)     — decode; leaders; block table; target validation.
//   Phase B (dom)      — CFG edges, RPO, iterative dominators, frontiers.
//   Phase C (emit)     — per-block lowering in pc order with a vreg -> node
//                        map; Phi placeholders at merge blocks (backedge
//                        inputs patched in finish); guards capture complete
//                        FrameStates.
//   Phase D (post)     — backedge patching, Phi type joins, guard list,
//                        OSR-capable blocks, dominance placement input.
//
// The value model is TAGGED everywhere (the same words T0 and J1 compute
// with): smi arithmetic asserts operands/result through guards and adds or
// compares the tagged words directly, so every fast-path op keeps T0-verbatim
// observable semantics (Rule 18) and the J2 win comes from SSA register
// allocation, folding, and redundancy removal rather than representation
// change. Deep untagging is the J3 strength-reduction domain; the Untag/Tag
// node pair added to the shared IR is where it will land.
#include "vortex/j2/graph_builder.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "vortex/runtime/object_model.hpp"
#include "vortex/support/containers.hpp"

namespace vortex::j2 {
namespace {

using ir::AccessKind;

using ir::CallShape;
using ir::CondCode;
using ir::GuardKind;
using ir::JType;
using ir::kNoNode;
using ir::NodeId;
using ir::NodeKind;
using ir::Node;
using ugb::Op;

class Builder {
public:
    Builder(const GraphBuilderParams& p, ir::Graph& graph, BuiltGraph& out)
        : p_(p), g_(graph), out_(out), method_(*p.method) {}

    bool run() {
        out_.register_count = method_.register_count;
        out_.argc = method_.arg_count;
        out_.method_id = method_.id;
        if (!scan()) return false;
        build_cfg();
        emit_blocks();
        if (failed_) return false;
        finish();
        return !failed_;
    }

    BuildError error() const { return error_; }
    uint32_t error_pc() const { return error_pc_; }

private:
    // ---- Phase A: scan ----------------------------------------------------
    struct Decoded {
        uint32_t pc;
        ugb::Instruction ins;
    };

    static bool is_branch(Op op) noexcept {
        return op == Op::JUMP || op == Op::JUMP_TRUE || op == Op::JUMP_FALSE ||
               op == Op::JUMP_EQ || op == Op::JUMP_NE;
    }
    static bool is_terminator(Op op) noexcept {
        return is_branch(op) || op == Op::RETURN || op == Op::RETURN_UNIT ||
               op == Op::UNREACHABLE;
    }

    bool scan() {
        ugb::InstructionStream stream(method_.code.data(), method_.code.size());
        size_t pc = 0;
        while (pc < method_.code.size()) {
            Decoded d;
            d.pc = static_cast<uint32_t>(pc);
            if (!stream.decode_at(pc, d.ins)) {
                return fail(BuildError::DecodeError, d.pc);
            }
            code_.push_back(d);
            insn_of_pc_.emplace(d.pc,
                                static_cast<uint32_t>(code_.size() - 1));
        }
        if (code_.empty()) return fail(BuildError::DecodeError, 0);
        // Leaders: entry, branch targets, instructions after terminators.
        leaders_.push_back(0);
        for (uint32_t i = 0; i < code_.size(); ++i) {
            const Op op = code_[i].ins.opcode;
            if (is_terminator(op) && i + 1 < code_.size()) {
                leaders_.push_back(code_[i + 1].pc);
            }
            if (is_branch(op)) {
                const auto it = insn_of_pc_.find(code_[i].ins.meta);
                if (it == insn_of_pc_.end() || code_[i].ins.meta == 0) {
                    if (it == insn_of_pc_.end()) {
                        return fail(BuildError::MalformedBranch,
                                    code_[i].pc);
                    }
                }
                if (it != insn_of_pc_.end()) {
                    leaders_.push_back(code_[i].ins.meta);
                }
            }
        }
        return true;
    }

    // ---- Phase B: CFG + dominators ----------------------------------------
    void edge(uint32_t from, uint32_t to) {
        out_.blocks[from].succs.push_back(to);
        out_.blocks[to].preds.push_back(from);
        if (from >= to) {
            backedge_sources_.push_back(to);
            backedge_source_of_[to] = from;
        }
    }

    void build_cfg() {
        std::sort(leaders_.begin(), leaders_.end());
        leaders_.erase(std::unique(leaders_.begin(), leaders_.end()),
                       leaders_.end());
        const std::vector<uint32_t>& leaders = leaders_;
        for (const uint32_t leader : leaders) {
            Block b;
            b.bytecode_begin = leader;
            block_at_[leader] = static_cast<uint32_t>(out_.blocks.size());
            out_.blocks.push_back(b);
        }
        block_of_insn_.assign(code_.size(), 0);
        uint32_t cur = 0;
        for (uint32_t i = 0; i < code_.size(); ++i) {
            const auto it = block_at_.find(code_[i].pc);
            if (it != block_at_.end()) cur = it->second;
            block_of_insn_[i] = cur;
            out_.blocks[cur].bytecode_end = code_[i].pc;
        }
        for (uint32_t i = 0; i < code_.size(); ++i) {
            const Op op = code_[i].ins.opcode;
            const uint32_t b = block_of_insn_[i];
            if (op == Op::JUMP) {
                edge(b, block_at_[code_[i].ins.meta]);
            } else if (op == Op::JUMP_TRUE || op == Op::JUMP_FALSE ||
                       op == Op::JUMP_EQ || op == Op::JUMP_NE) {
                edge(b, block_at_[code_[i].ins.meta]);
                if (i + 1 < code_.size()) edge(b, block_of_insn_[i + 1]);
            } else if (!is_terminator(op)) {
                if (i + 1 < code_.size() && block_of_insn_[i + 1] != b) {
                    edge(b, block_of_insn_[i + 1]);
                }
            }
        }
        // Post-order DFS from entry, then reverse for RPO.
        visited_.assign(out_.blocks.size(), false);
        rpo_visit(0);
        for (uint32_t i = 0; i < out_.blocks.size(); ++i) {
            if (!visited_[i]) rpo_visit(i);  // unreachable: tail order
        }
        std::reverse(rpo_.begin(), rpo_.end());
        // Immediate dominators (Cooper-Harvey-Kennedy over RPO).
        idom_.assign(out_.blocks.size(), 0);
        rpo_index_.assign(out_.blocks.size(), UINT32_MAX);
        for (size_t i = 0; i < rpo_.size(); ++i) rpo_index_[rpo_[i]] = i;
        idom_[rpo_[0]] = rpo_[0];
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t bi = 1; bi < rpo_.size(); ++bi) {
                const uint32_t b = rpo_[bi];
                uint32_t new_idom = UINT32_MAX;
                for (const uint32_t pred : out_.blocks[b].preds) {
                    if (rpo_index_[pred] == UINT32_MAX) continue;
                    if (pred == b) continue;
                    new_idom = (new_idom == UINT32_MAX)
                                   ? pred
                                   : intersect(pred, new_idom);
                }
                if (new_idom != UINT32_MAX && idom_[b] != new_idom) {
                    idom_[b] = new_idom;
                    changed = true;
                }
            }
        }
        // Backedge set: sorted+unique.
        std::sort(backedge_sources_.begin(), backedge_sources_.end());
        backedge_sources_.erase(
            std::unique(backedge_sources_.begin(), backedge_sources_.end()),
            backedge_sources_.end());
        // Per-block instruction ranges (blocks partition the decode order).
        // The sentinel must NOT be {0,0}: a block legitimately starting at
        // instruction 0 would have its `first` clobbered to 1, silently
        // dropping the method's first instruction from lowering.
        block_insn_range_.assign(out_.blocks.size(),
                                 {UINT32_MAX, UINT32_MAX});
        for (uint32_t i = 0; i < code_.size(); ++i) {
            auto& range = block_insn_range_[block_of_insn_[i]];
            if (range.first == UINT32_MAX) range.first = i;
            range.second = i;
        }
        // Dominance frontiers.
        df_.assign(out_.blocks.size(), {});
        for (uint32_t b = 0; b < out_.blocks.size(); ++b) {
            if (out_.blocks[b].preds.size() < 2) continue;
            for (const uint32_t pred : out_.blocks[b].preds) {
                if (rpo_index_[pred] == UINT32_MAX) continue;
                uint32_t runner = pred;
                while (runner != idom_[b] && runner != b) {
                    df_[runner].push_back(b);
                    if (runner == idom_[runner]) break;
                    runner = idom_[runner];
                }
            }
        }
    }

    uint32_t intersect(uint32_t a, uint32_t b) {
        while (a != b) {
            while (rpo_index_[a] > rpo_index_[b]) a = idom_[a];
            while (rpo_index_[b] > rpo_index_[a]) b = idom_[b];
        }
        return a;
    }

    void rpo_visit(uint32_t b) {
        visited_[b] = true;
        for (const uint32_t succ : out_.blocks[b].succs) {
            if (!visited_[succ]) rpo_visit(succ);
        }
        rpo_.push_back(b);
    }

    // ---- Phase C: emit -----------------------------------------------------
    void emit_blocks() {
        block_exit_.assign(out_.blocks.size(), {});
        block_exit_effect_.assign(out_.blocks.size(), kNoNode);
        block_emitted_.assign(out_.blocks.size(), false);
        out_.block_entry_values.assign(out_.blocks.size(), {});
        // FORWARD RPO: a single-predecessor block copies its pred's exit
        // values, so the pred must be emitted first (it dominates, hence
        // precedes in RPO). Loop backedges are the one case where a pred
        // lags behind — those are multi-pred headers whose Phi placeholders
        // get patched in finish().
        for (size_t i = 0; i < rpo_.size(); ++i) {
            emit_block(rpo_[i]);
            if (failed_) return;
        }
    }

    void emit_block(uint32_t bi) {
        Block& blk = out_.blocks[bi];
        cur_block_ = bi;
        const size_t n_preds = blk.preds.size();
        const bool is_header = std::binary_search(backedge_sources_.begin(),
                                                  backedge_sources_.end(),
                                                  bi);
        if (bi == 0) {
            blk.region = g_.add(NodeKind::Start, {});
        } else if (is_header || n_preds > 1) {
            // Multi-pred merge / loop header: the Region's control input
            // stays unset here (the CFG edge list is the true predecessor
            // record); the immediate dominator link is assigned below so
            // DCE reachability walks the dominator tree.
            blk.region = g_.add(NodeKind::Region, {});
            g_.node(blk.region).control =
                out_.blocks[idom_[bi]].region;
        } else {
            blk.region = g_.add(NodeKind::Region, {});
            // Single pred: hang off the pred's actual control terminator
            // when one exists (the If that selects this arm). Wiring to the
            // immediate dominator instead would bypass the conditional chain
            // entirely — DCE's backward walk (Return -> Region -> control)
            // then never reaches the If, kills it, and every value computed
            // only on the conditional path dies with it (Rule 51: the
            // reachability set must cover the control chain).
            const uint32_t pred = blk.preds[0];
            const NodeId pred_end = out_.blocks[pred].control_end;
            g_.node(blk.region).control =
                (pred_end != kNoNode &&
                 g_.node(pred_end).kind == NodeKind::If)
                    ? pred_end
                    : out_.blocks[idom_[bi]].region;
        }
        set_node_block(blk.region, bi);
        if (bi == 0) {
            emit_entry_params();
        } else if (n_preds == 1) {
            vreg_ = block_exit_[blk.preds[0]];
        } else {
            vreg_.assign(out_.register_count, kNoNode);
            merge_predecessor_values(bi);
        }
        effect_ = kNoNode;
        if (n_preds >= 1) {
            for (const uint32_t pred : blk.preds) {
                if (block_exit_effect_[pred] != kNoNode &&
                    rpo_index_[pred] != UINT32_MAX) {
                    effect_ = block_exit_effect_[pred];
                    // Loop-carried effects: when the pred is a loop header,
                    // the backedge body's effects ALSO execute before this
                    // block (every iteration after the first). Chaining to
                    // the body's exit effect keeps stores inside the loop on
                    // the effect chain — otherwise DCE's backward walk
                    // (Return -> effect_in) never reaches them and kills
                    // effectful nodes the program observably depends on
                    // (Rule 51: reachability must cover the effect chain).
                    const auto it = backedge_source_of_.find(pred);
                    if (it != backedge_source_of_.end() &&
                        block_emitted_[it->second] &&
                        block_exit_effect_[it->second] != kNoNode) {
                        effect_ = block_exit_effect_[it->second];
                    }
                    break;
                }
            }
        }
        const auto range = insn_range_of_block(bi);
        for (uint32_t i = range.first; i <= range.second; ++i) {
            if (block_of_insn_[i] != bi) continue;
            lower(i);
            if (failed_) return;
            if (g_.node_count() > p_.node_cap) {
                fail(BuildError::BudgetExceeded, code_[i].pc);
                return;
            }
        }
        out_.block_entry_values[bi] = vreg_;
        block_exit_[bi] = vreg_;
        block_exit_effect_[bi] = effect_;
        block_emitted_[bi] = true;
    }

    std::pair<uint32_t, uint32_t> insn_range_of_block(uint32_t bi) const {
        // Blocks own a contiguous instruction range (leaders partition the
        // decode order), so a block's range is derivable from its first and
        // last instruction; a linear precompute keeps this O(n) overall.
        return block_insn_range_[bi];
    }

    void merge_predecessor_values(uint32_t bi) {
        Block& blk = out_.blocks[bi];
        if (blk.preds.empty()) return;
        for (uint32_t v = 0; v < out_.register_count; ++v) {
            NodeId first = kNoNode;
            bool all_same = true;
            bool any_pending = false;  // a backedge pred not yet emitted
            for (const uint32_t pred : blk.preds) {
                if (!block_emitted_[pred]) {
                    any_pending = true;
                    continue;
                }
                const NodeId pv = pred_exit_value(pred, v);
                if (pv == kNoNode) continue;
                if (first == kNoNode) {
                    first = pv;
                } else if (first != pv) {
                    all_same = false;
                }
            }
            // A pending (backedge) pred ALWAYS forces a Phi placeholder:
            // the header must still bind the vreg (leaving it unbound made
            // every downstream read see kNoNode — the loop body computed
            // from undefined). finish() patches the pending slot.
            if (any_pending || (!all_same && first != kNoNode)) {
                std::vector<NodeId> inputs;
                inputs.reserve(blk.preds.size());
                for (const uint32_t pred : blk.preds) {
                    if (!block_emitted_[pred]) {
                        inputs.push_back(kNoNode);  // patched in finish()
                    } else {
                        inputs.push_back(pred_exit_value(pred, v));
                    }
                }
                const NodeId phi = g_.add(NodeKind::Phi, {});
                Node& pn = g_.node(phi);
                pn.control = blk.region;
                pn.data_inputs.clear();
                for (const NodeId in : inputs) pn.data_inputs.push_back(in);
                set_node_block(phi, bi);
                set_type(phi, JType::Unknown);
                vreg_[v] = phi;
                pending_phis_.push_back({phi, bi, v});
            } else {
                vreg_[v] = first;
            }
        }
    }

    NodeId pred_exit_value(uint32_t pred, uint32_t v) const {
        if (pred >= block_exit_.size() || block_exit_[pred].empty()) {
            return kNoNode;
        }
        return block_exit_[pred][v];
    }

    void emit_entry_params() {
        // The entry ABI materializes the whole T0 register file (the J1
        // entry/OSR convention), so every register is a Parameter; the
        // argument window is the first argc registers.
        vreg_.assign(out_.register_count, kNoNode);
        for (uint32_t v = 0; v < out_.register_count; ++v) {
            const NodeId prm = g_.add(NodeKind::Parameter, {});
            set_node_block(prm, 0);
            set_type(prm, JType::Unknown);
            vreg_[v] = prm;
        }
    }

    // ---- instruction lowering ----------------------------------------------
    void lower(uint32_t idx) {
        const ugb::Instruction& ins = code_[idx].ins;
        const uint32_t pc = code_[idx].pc;
        if (getenv("VORTEX_BUILDER_TRACE")) {
            fprintf(stderr, "[lower] idx=%u pc=%u op=%d dst=%u src0=%u\n",
                    idx, pc, (int)ins.opcode, ins.dst,
                    ins.srcs.empty() ? 0 : ins.srcs[0]);
        }
        cur_pc_ = pc;
        cur_insn_index_ = idx;
        cur_frame_state_ = kNoNode;
        switch (ins.opcode) {
        case Op::CONST_NULL:
            bind_dst(ins, make_const(static_cast<int64_t>(kNullBits),
                                     JType::Null));
            break;
        case Op::CONST_UNDEFINED:
            bind_dst(ins, make_const(static_cast<int64_t>(kUndefinedBits),
                                     JType::Unknown));
            break;
        case Op::CONST_FALSE:
            bind_dst(ins, make_const(kFalseBits, JType::Bool));
            break;
        case Op::CONST_TRUE:
            bind_dst(ins, make_const(static_cast<int64_t>(kTrueBits),
                                     JType::Bool));
            break;
        case Op::CONST_I32:
            bind_dst(ins,
                     make_const(static_cast<int64_t>(
                                    static_cast<int32_t>(ins.meta)) ,
                                JType::Smi));
            break;
        case Op::CONST_I64:
            bind_dst(ins, const_pool_load(ins.meta));
            break;
        case Op::CONST_F64:
            // T0 allocates a fresh boxed double per execution: identity is
            // observable, so allocations are never folded (Rule 108).
            bind_dst(ins, alloc_double(load_f64_const_bits(ins.meta)));
            break;
        case Op::CONST_STRING: case Op::CONST_METHOD:
            bind_dst(ins, const_pool_load(ins.meta));
            break;
        case Op::MOVE: case Op::COPY:
            bind_dst(ins, src(ins, 0));
            break;
        case Op::SWAP: {
            const NodeId a = src(ins, 0);
            const NodeId b = src(ins, 1);
            bind_vreg(ins.dst, b);
            bind_vreg(ins.srcs[1], a);
            break;
        }
        case Op::ADD_I32: case Op::ADD_I64: case Op::ADD_CHECKED_I32:
        case Op::SUB_I32: case Op::SUB_I64:
        case Op::MUL_I32: case Op::MUL_I64:
            lower_typed_smi_arith(ins);
            break;
        case Op::ADD_ANY: case Op::SUB_ANY: case Op::MUL_ANY:
            lower_generic_binop(ins);
            break;
        case Op::ADD_F64: case Op::SUB_F64: case Op::MUL_F64:
        case Op::DIV_F64:
            lower_f64_binop(ins);
            break;
        case Op::DIV_S_I64: case Op::REM_S_I64:
        case Op::DIV_U_I64: case Op::REM_U_I64:
            lower_div_rem(ins);
            break;
        case Op::NEG_I64:
            lower_neg_i64(ins);
            break;
        case Op::NEG_F64:
            lower_neg_f64(ins);
            break;
        case Op::AND_I: case Op::OR_I: case Op::XOR_I:
            lower_logical_bitop(ins);
            break;
        case Op::SHL_I: case Op::SHR_S_I: case Op::SHR_U_I:
            lower_shift(ins);
            break;
        case Op::EQ_I32: case Op::EQ_I64: case Op::NE_I64:
        case Op::LT_S_I64: case Op::LE_S_I64: case Op::GT_S_I64:
        case Op::GE_S_I64:
            lower_smi_compare(ins);
            break;
        case Op::EQ_F64: case Op::LT_F64: case Op::LE_F64:
        case Op::GT_F64: case Op::GE_F64:
            lower_f64_compare(ins);
            break;
        case Op::EQ_REF: case Op::NE_REF: case Op::EQ_ANY:
        case Op::COMPARE_ANY:
            lower_generic_compare(ins);
            break;
        case Op::EQ_NULL:
            bind_dst(ins, make_compare(src(ins, 0),
                                       make_const(static_cast<int64_t>(kNullBits), JType::Null),
                                       CondCode::Eq));
            break;
        case Op::I64_TO_F64:
            lower_i64_to_f64(ins);
            break;
        case Op::F64_TO_I64:
            lower_generic_unop(ins);
            break;
        case Op::SEXT_I32_I64: case Op::TRUNC_I64_I32:
            // Both are identities on the tagged smi representation after
            // the operand is proven to be a smi (T0-verbatim).
            bind_dst(ins, smi_guard(src(ins, 0)));
            break;
        case Op::BOX: case Op::UNBOX: case Op::ANY_TO_TYPED:
        case Op::TYPED_TO_ANY:
            // Outside the J1 corpus coverage: the caller keeps the method
            // on J1 (Rule 11 fallback, Rule 76 telemetry via the reason).
            fail(BuildError::UnsupportedOpcode, pc);
            break;
        case Op::JUMP:
            branch_to(ins.meta);
            break;
        case Op::JUMP_TRUE: case Op::JUMP_FALSE: {
            // The branch consumes the condition through the backend's
            // T0-verbatim truthiness (TaggedValue::truthy: smi payload
            // != 0; the false/null/undefined words are falsey; heap
            // objects truthy). A wrapper compare here would test the
            // tagged word against a bogus constant — removed.
            bind_branch(ins, src(ins, 0));
            break;
        }
        case Op::JUMP_EQ: case Op::JUMP_NE: {
            const NodeId cmp =
                make_compare(src(ins, 0), src(ins, 1),
                             ins.opcode == Op::JUMP_EQ ? CondCode::Eq
                                                       : CondCode::Ne);
            const NodeId iff =
                g_.add(NodeKind::If, {cmp}, cur_region());
            set_node_block(iff, cur_block_);
            Block& blk = out_.blocks[cur_block_];
            blk.control_end = iff;
            if (p_.profiles != nullptr &&
                cur_insn_index_ < p_.profiles->size()) {
                const ugb::ProfileSlot& prof = (*p_.profiles)[cur_insn_index_];
                const uint64_t total = prof.branch_counts[0] +
                                       prof.branch_counts[1];
                if (total >= kMinBranchSamples) {
                    uint64_t permille =
                        (prof.branch_counts[1] * 1000) / total;
                    permille = std::clamp<uint64_t>(permille, 1, 999);
                    blk.true_prob_permille =
                        static_cast<uint32_t>(permille);
                }
            }
            blk.target_is_true_edge = true;
            branch_target_ = ins.meta;
            break;
        }
        case Op::RETURN:
            bind_return(src(ins, 0));
            break;
        case Op::RETURN_UNIT:
            bind_return(make_const(static_cast<int64_t>(kNullBits),
                                   JType::Null));
            break;
        case Op::UNREACHABLE:
            fail(BuildError::UnsupportedOpcode, pc);
            break;
        case Op::CALL_DIRECT: case Op::CALL_VIRTUAL:
        case Op::CALL_INTERFACE: case Op::CALL_DYNAMIC:
        case Op::CALL_CLOSURE: case Op::CALL_BUILTIN:
            lower_call(ins);
            break;
        case Op::TAIL_CALL_DIRECT:
            fail(BuildError::UnsupportedOpcode, pc);
            break;
        case Op::NEW_OBJECT:
            lower_new_object(ins);
            break;
        case Op::NEW_ARRAY:
            lower_new_array(ins);
            break;
        case Op::GET_FIELD: case Op::GET_FIELD_SHAPE:
            lower_get_field(ins);
            break;
        case Op::SET_FIELD: case Op::SET_FIELD_SHAPE:
            lower_set_field(ins);
            break;
        case Op::GET_PROP_DYNAMIC: case Op::SET_PROP_DYNAMIC:
        case Op::DELETE_PROP_DYNAMIC: case Op::HAS_PROP_DYNAMIC:
            fail(BuildError::UnsupportedOpcode, pc);
            break;
        case Op::ARRAY_LENGTH: {
            const NodeId arr = heap_guard(src(ins, 0));
            const NodeId len =
                g_.add_aux(NodeKind::Load, {arr, frame_state()},
                           static_cast<uint32_t>(AccessKind::ArrayLength),
                           cur_region(), effect_);
            chain_effect(len);
            set_node_block(len, cur_block_);
            set_type(len, JType::Smi);
            bind_dst(ins, len);
            break;
        }
        case Op::ARRAY_GET: case Op::ARRAY_GET_UNCHECKED: {
            const bool checked = ins.opcode == Op::ARRAY_GET;
            const NodeId arr = heap_guard(src(ins, 0));
            const NodeId index = checked ? smi_guard(src(ins, 1))
                                         : src(ins, 1);
            const NodeId ld = g_.add_aux(
                NodeKind::Load, {arr, index, frame_state()},
                static_cast<uint32_t>(AccessKind::ArrayElement),
                cur_region(), effect_);
            chain_effect(ld);
            set_node_block(ld, cur_block_);
            set_type(ld, JType::Unknown);
            bind_dst(ins, ld);
            break;
        }
        case Op::ARRAY_SET: case Op::ARRAY_SET_UNCHECKED: {
            const bool checked = ins.opcode == Op::ARRAY_SET;
            const NodeId arr = heap_guard(src(ins, 0));
            const NodeId index = checked ? smi_guard(src(ins, 1))
                                         : src(ins, 1);
            const NodeId value = src(ins, 2);
            const NodeId st = g_.add_aux(
                NodeKind::Store, {arr, index, value, frame_state()},
                static_cast<uint32_t>(AccessKind::ArrayElement),
                cur_region(), effect_);
            chain_effect(st);
            set_node_block(st, cur_block_);
            break;
        }
        case Op::CHECK_CLASS: {
            const NodeId v = heap_guard(src(ins, 0));
            const NodeId st = frame_state();
            const NodeId gd =
                g_.add_aux(NodeKind::ClassGuard, {v, st}, ins.meta,
                           cur_region());
            set_node_block(gd, cur_block_);
            set_type(gd, JType::Ref);
            out_.guards.push_back(gd);
            bind_dst(ins, gd);
            break;
        }
        case Op::CHECK_NULL: {
            // Passes when the value IS null; deopt into T0 otherwise (T0
            // raises the same error — Rule 18).
            const NodeId st = frame_state();
            const NodeId gd = g_.add_aux(NodeKind::TypeGuard,
                                         {src(ins, 0), st},
                                         static_cast<uint32_t>(GuardKind::IsNull),
                                         cur_region());
            set_node_block(gd, cur_block_);
            set_type(gd, JType::Null);
            out_.guards.push_back(gd);
            bind_dst(ins, gd);
            break;
        }
        case Op::CHECK_NON_NULL: {
            const NodeId v = heap_guard(src(ins, 0));
            const NodeId st = frame_state();
            const NodeId gd = g_.add_aux(
                NodeKind::TypeGuard, {v, st},
                static_cast<uint32_t>(GuardKind::IsNonNull), cur_region());
            set_node_block(gd, cur_block_);
            set_type(gd, JType::Ref);
            out_.guards.push_back(gd);
            bind_dst(ins, gd);
            break;
        }
        case Op::CHECK_BOUNDS: {
            const NodeId st = frame_state();
            const NodeId gd = g_.add_aux(
                NodeKind::BoundsGuard,
                {smi_guard(src(ins, 0)), smi_guard(src(ins, 1)), st}, 0,
                cur_region());
            set_node_block(gd, cur_block_);
            out_.guards.push_back(gd);
            break;
        }
        case Op::CHECK_SHAPE:
            fail(BuildError::UnsupportedOpcode, pc);
            break;
        case Op::ATOMIC_LOAD: case Op::ATOMIC_STORE: case Op::FENCE_SEQ_CST:
            fail(BuildError::UnsupportedOpcode, pc);
            break;
        case Op::SAFEPOINT_POLL: {
            const NodeId sp = g_.add(NodeKind::Safepoint, {}, cur_region(),
                                     effect_);
            chain_effect(sp);
            set_node_block(sp, cur_block_);
            break;
        }
        case Op::CLOSURE_NEW: case Op::CLOSURE_GET_UPVALUE:
        case Op::CLOSURE_SET_UPVALUE:
        case Op::TRY_BEGIN: case Op::TRY_END: case Op::THROW:
        case Op::POLY_EXECUTE: case Op::POLY_READ: case Op::POLY_WRITE:
        case Op::POLY_SEND:
        case Op::DEBUG_TRAP: case Op::EXT_OP:
            fail(BuildError::UnsupportedOpcode, pc);
            break;
        case Op::WRITE_BARRIER_STORE:
            // A hint over an ordinary reference store: the canonical SET_FIELD
            // lowering already carries the barrier (docs/ir-son-ciog.md 4).
            break;
        case Op::DEBUG_SRCPOS:
            break;  // no code (observability binds at emission, M6)
        case Op::ILLEGAL:
        default:
            fail(BuildError::UnsupportedOpcode, pc);
            break;
        }
    }

    // ---- lowering helpers ---------------------------------------------------

    void lower_typed_smi_arith(const ugb::Instruction& ins) {
        const NodeId a = smi_guard(src(ins, 0));
        const NodeId b = smi_guard(src(ins, 1));
        NodeKind k = NodeKind::Add;
        switch (ins.opcode) {
        case Op::SUB_I32: case Op::SUB_I64: k = NodeKind::Sub; break;
        case Op::MUL_I32: case Op::MUL_I64: k = NodeKind::Mul; break;
        default: k = NodeKind::Add; break;
        }
        const NodeId raw = g_.add(k, {a, b}, cur_region());
        set_node_block(raw, cur_block_);
        // Exactness: smi add/sub/mul must not overflow the int63 payload.
        // Overflow deopts to T0, which raises the canonical overflow error
        // — observably identical to executing the op there (Rule 18).
        const NodeId st = frame_state();
        const NodeId gd = g_.add(NodeKind::OverflowGuard, {raw, st},
                                 cur_region());
        set_node_block(gd, cur_block_);
        set_type(gd, JType::Smi);
        out_.guards.push_back(gd);
        bind_dst(ins, gd);
    }

    void lower_generic_binop(const ugb::Instruction& ins) {
        call_helper(ins, static_cast<uint32_t>(ins.opcode), 2);
    }

    void lower_generic_unop(const ugb::Instruction& ins) {
        call_helper(ins, static_cast<uint32_t>(ins.opcode), 1);
    }

    void lower_generic_compare(const ugb::Instruction& ins) {
        call_helper(ins, static_cast<uint32_t>(ins.opcode), 2);
    }

    /// Canonical/generic forms run through the shared canonical-binop helper
    /// (T0 semantics verbatim). Result type is Unknown — the helper decides.
    void call_helper(const ugb::Instruction& ins, uint32_t op_id,
                     uint32_t argc) {
        // Shape::Generic — the shared canonical-binop helper (generic_binop
        // ABI: ctx, op id, a, b). The op id rides aux; inputs are the
        // operand values + the FrameState (the call is a GC safepoint).
        std::vector<NodeId> inputs;
        inputs.reserve(argc + 1);
        for (uint32_t i = 0; i < argc; ++i) {
            inputs.push_back(src(ins, i));
        }
        inputs.push_back(frame_state());
        const NodeId call = g_.add_aux(NodeKind::Call, {}, op_id,
                                       cur_region(), effect_);
        Node& cn = g_.node(call);
        cn.data_inputs.clear();
        for (const NodeId in : inputs) cn.data_inputs.push_back(in);
        cn.aux = (static_cast<uint32_t>(CallShape::Generic) << 16) | op_id;
        cn.const_value = static_cast<int64_t>(ins.dst);  // deopt continuation
        cn.control = cur_region();
        chain_effect(call);
        set_node_block(call, cur_block_);
        set_type(call, JType::Unknown);
        out_.guards.push_back(call);  // deopt point (GC map) — see finish
        bind_dst(ins, call);
    }

    void lower_f64_binop(const ugb::Instruction& ins) {
        const NodeId a = f64_guard(src(ins, 0));
        const NodeId b = f64_guard(src(ins, 1));
        const NodeId fa = payload_load(a);
        const NodeId fb = payload_load(b);
        NodeKind k = NodeKind::FAdd;
        switch (ins.opcode) {
        case Op::SUB_F64: k = NodeKind::FSub; break;
        case Op::MUL_F64: k = NodeKind::FMul; break;
        case Op::DIV_F64: k = NodeKind::FDiv; break;
        default: k = NodeKind::FAdd; break;
        }
        const NodeId arith = g_.add(k, {fa, fb}, cur_region());
        set_node_block(arith, cur_block_);
        bind_dst(ins, alloc_double(arith));
    }

    void lower_div_rem(const ugb::Instruction& ins) {
        if (ins.opcode == Op::DIV_U_I64 || ins.opcode == Op::REM_U_I64) {
            // The shared helper implements only the signed forms (T0 has no
            // unsigned-div handler in M0 — such methods never reach J2); a
            // named refusal beats routing to the helper's default arm
            // (Rule 76: no silent wrong semantics).
            error_ = BuildError::UnsupportedOpcode;
            return;
        }
        const NodeId a = smi_guard(src(ins, 0));
        const NodeId b = smi_guard(src(ins, 1));
        const NodeKind k = (ins.opcode == Op::DIV_S_I64 ||
                            ins.opcode == Op::DIV_U_I64)
                               ? NodeKind::Div
                               : NodeKind::Rem;
        const NodeId d = g_.add_aux(k, {a, b},
                                    static_cast<uint32_t>(ins.opcode),
                                    cur_region());
        set_node_block(d, cur_block_);
        set_type(d, JType::Smi);
        // aux carries the UGB OPCODE (the emitter routes the node to the
        // shared generic-binop helper, which decides signed/unsigned and
        // the trap semantics from it). DivSignedness was previously stored
        // here and the emitter passed it as the op id — every Div/Rem ran
        // as op 0 (silent miscompile; now locked by
        // j2_div_helper_call_keeps_live_values). Trap checks (divisor 0;
        // min / -1 signed) live in the helper, T0-verbatim.
        bind_dst(ins, d);
    }

    void lower_neg_i64(const ugb::Instruction& ins) {
        const NodeId a = smi_guard(src(ins, 0));
        const NodeId n = g_.add(NodeKind::Neg, {a}, cur_region());
        set_node_block(n, cur_block_);
        set_type(n, JType::Smi);
        bind_dst(ins, n);
    }

    void lower_neg_f64(const ugb::Instruction& ins) {
        const NodeId a = f64_guard(src(ins, 0));
        const NodeId fa = payload_load(a);
        const NodeId n = g_.add(NodeKind::FNeg, {fa}, cur_region());
        set_node_block(n, cur_block_);
        bind_dst(ins, alloc_double(n));
    }

    void lower_logical_bitop(const ugb::Instruction& ins) {
        // AND/OR/XOR of two tagged smis commutes with the tag bit
        // ((a<<1)&(b<<1) == (a&b)<<1), and the int63 payloads cannot
        // overflow, so these run on the tagged words with smi guards only.
        const NodeId a = smi_guard(src(ins, 0));
        const NodeId b = smi_guard(src(ins, 1));
        NodeKind k = NodeKind::And;
        if (ins.opcode == Op::OR_I) k = NodeKind::Or;
        if (ins.opcode == Op::XOR_I) k = NodeKind::Xor;
        const NodeId r = g_.add(k, {a, b}, cur_region());
        set_node_block(r, cur_block_);
        set_type(r, JType::Smi);
        bind_dst(ins, r);
    }

    void lower_shift(const ugb::Instruction& ins) {
        // Shifts operate on the UNtagged payload (T0: a = as_smi, shift =
        // b & kI64ShiftMask); the result re-enters the tagged world. SHL can
        // leave the smi range: T0 traps, so emission carries the range check
        // (the graph records the pure arithmetic only).
        const NodeId a = smi_guard(src(ins, 0));
        const NodeId b = smi_guard(src(ins, 1));
        const NodeId ua = g_.add(NodeKind::Untag, {a}, cur_region());
        const NodeId ub = g_.add(NodeKind::Untag, {b}, cur_region());
        set_node_block(ua, cur_block_);
        set_node_block(ub, cur_block_);
        set_type(ua, JType::Unknown);  // raw int63 payload
        set_type(ub, JType::Unknown);
        // Shift kind rides aux: 0 = SHL, 1 = SAR, 2 = SHR (the emission
        // contract maps them onto the x64 group; T0 semantics per kind).
        uint32_t shift_kind = 0;
        if (ins.opcode == Op::SHR_S_I) shift_kind = 1;
        if (ins.opcode == Op::SHR_U_I) shift_kind = 2;
        const NodeId sh = g_.add_aux(NodeKind::Shift, {ua, ub}, shift_kind,
                                     cur_region());
        set_node_block(sh, cur_block_);
        const NodeId tagged = g_.add(NodeKind::Tag, {sh}, cur_region());
        set_node_block(tagged, cur_block_);
        set_type(tagged, JType::Smi);
        bind_dst(ins, tagged);
    }

    void lower_smi_compare(const ugb::Instruction& ins) {
        const NodeId a = smi_guard(src(ins, 0));
        const NodeId b = smi_guard(src(ins, 1));
        // Tagged smi words preserve equality and signed order (payloads are
        // doubled), so the compare runs on the tagged words directly.
        CondCode cc = CondCode::Eq;
        switch (ins.opcode) {
        case Op::NE_I64: cc = CondCode::Ne; break;
        case Op::LT_S_I64: cc = CondCode::LtS; break;
        case Op::LE_S_I64: cc = CondCode::LeS; break;
        case Op::GT_S_I64: cc = CondCode::GtS; break;
        case Op::GE_S_I64: cc = CondCode::GeS; break;
        default: cc = CondCode::Eq; break;
        }
        bind_dst(ins, make_compare(a, b, cc));
    }

    void lower_f64_compare(const ugb::Instruction& ins) {
        const NodeId a = f64_guard(src(ins, 0));
        const NodeId b = f64_guard(src(ins, 1));
        const NodeId fa = payload_load(a);
        const NodeId fb = payload_load(b);
        CondCode cc = CondCode::LtF;
        switch (ins.opcode) {
        case Op::EQ_F64: cc = CondCode::Eq; break;
        case Op::LE_F64: cc = CondCode::LeF; break;
        case Op::GT_F64: cc = CondCode::GtF; break;
        case Op::GE_F64: cc = CondCode::GeF; break;
        default: cc = CondCode::LtF; break;
        }
        bind_dst(ins, make_compare(fa, fb, cc));
    }

    void lower_i64_to_f64(const ugb::Instruction& ins) {
        const NodeId a = smi_guard(src(ins, 0));
        const NodeId ua = g_.add(NodeKind::Untag, {a}, cur_region());
        set_node_block(ua, cur_block_);
        const NodeId f = g_.add(NodeKind::IToF, {ua}, cur_region());
        set_node_block(f, cur_block_);
        bind_dst(ins, alloc_double(f));
    }

    void lower_call(const ugb::Instruction& ins) {
        // arg window: vregs [s0 .. s0 + argc); argc arrives in src1 as a
        // literal (J1 PA_CallArgc convention). Argument values become call
        // data inputs so regalloc keeps them live across the call.
        const uint32_t argc =
            ins.srcs.size() > 1 ? ins.srcs[1] : 0;
        std::vector<NodeId> inputs;
        inputs.reserve(argc + 2);
        for (uint32_t i = 0; i < argc; ++i) {
            inputs.push_back(vreg_value(ins.srcs[0] + i));
        }
        const NodeId st = frame_state();  // calls are GC safepoints
        inputs.push_back(st);
        const uint32_t callee_token = ins.meta;
        const NodeId call = g_.add_aux(NodeKind::Call, {}, callee_token,
                                       cur_region(), effect_);
        Node& cn = g_.node(call);
        cn.data_inputs.clear();
        for (const NodeId in : inputs) cn.data_inputs.push_back(in);
        CallShape shape = CallShape::Direct;
        if (ins.opcode == Op::CALL_BUILTIN) shape = CallShape::Builtin;
        if (ins.opcode == Op::CALL_VIRTUAL ||
            ins.opcode == Op::CALL_INTERFACE ||
            ins.opcode == Op::CALL_DYNAMIC ||
            ins.opcode == Op::CALL_CLOSURE) {
            shape = CallShape::Virtual;
        }
        cn.aux = (static_cast<uint32_t>(shape) << 16) | callee_token;
        cn.const_value = static_cast<int64_t>(ins.dst);  // deopt continuation
        cn.control = cur_region();
        chain_effect(call);
        set_node_block(call, cur_block_);
        set_type(call, JType::Unknown);
        out_.guards.push_back(call);
        bind_vreg(ins.dst, call);
    }

    void lower_new_object(const ugb::Instruction& ins) {
        const NodeId alloc = g_.add_aux(NodeKind::Allocate, {}, ins.meta,
                                        cur_region(), effect_);
        chain_effect(alloc);
        set_node_block(alloc, cur_block_);
        set_type(alloc, JType::Ref);
        bind_dst(ins, alloc);
    }

    void lower_new_array(const ugb::Instruction& ins) {
        const NodeId len = smi_guard(src(ins, 0));
        const NodeId alloc =
            g_.add_aux(NodeKind::Allocate, {len}, 1, cur_region(), effect_);
        chain_effect(alloc);
        set_node_block(alloc, cur_block_);
        set_type(alloc, JType::Ref);
        bind_dst(ins, alloc);
    }

    void lower_get_field(const ugb::Instruction& ins) {
        const NodeId obj = heap_guard(src(ins, 0));
        const NodeId ld =
            g_.add_aux(NodeKind::Load, {obj, frame_state()},
                       static_cast<uint32_t>(AccessKind::Field),
                       cur_region(), effect_);
        Node& ln = g_.node(ld);
        ln.const_value = ins.meta;  // field token rides const_value (aux is
                                    // the AccessKind discriminator)
        chain_effect(ld);
        set_node_block(ld, cur_block_);
        set_type(ld, JType::Unknown);
        bind_dst(ins, ld);
    }

    void lower_set_field(const ugb::Instruction& ins) {
        const NodeId obj = heap_guard(src(ins, 0));
        const NodeId st =
            g_.add_aux(NodeKind::Store, {obj, src(ins, 1), frame_state()},
                       static_cast<uint32_t>(AccessKind::Field),
                       cur_region(), effect_);
        Node& sn = g_.node(st);
        sn.const_value = ins.meta;
        chain_effect(st);
        set_node_block(st, cur_block_);
    }

    // ---- value helpers ------------------------------------------------------

    NodeId smi_guard(NodeId v) {
        if (type_of(v) == JType::Smi || type_of(v) == JType::Bool) return v;
        const NodeId st = frame_state();
        const NodeId gd =
            g_.add_aux(NodeKind::TypeGuard, {v, st},
                       static_cast<uint32_t>(GuardKind::Smi), cur_region());
        set_node_block(gd, cur_block_);
        set_type(gd, JType::Smi);
        out_.guards.push_back(gd);
        return gd;
    }

    NodeId heap_guard(NodeId v) {
        if (type_of(v) == JType::Ref || type_of(v) == JType::BoxedF64) {
            return v;
        }
        const NodeId st = frame_state();
        const NodeId gd =
            g_.add_aux(NodeKind::TypeGuard, {v, st},
                       static_cast<uint32_t>(GuardKind::Heap), cur_region());
        set_node_block(gd, cur_block_);
        set_type(gd, JType::Ref);
        out_.guards.push_back(gd);
        return gd;
    }

    NodeId f64_guard(NodeId v) {
        if (type_of(v) == JType::BoxedF64) return v;
        const NodeId st = frame_state();
        const NodeId gd =
            g_.add_aux(NodeKind::TypeGuard, {v, st},
                       static_cast<uint32_t>(GuardKind::BoxedF64),
                       cur_region());
        set_node_block(gd, cur_block_);
        set_type(gd, JType::BoxedF64);
        out_.guards.push_back(gd);
        return gd;
    }

    NodeId payload_load(NodeId boxed) {
        const NodeId ld =
            g_.add_aux(NodeKind::Load, {boxed},
                       static_cast<uint32_t>(AccessKind::DoublePayload),
                       cur_region(), effect_);
        chain_effect(ld);
        set_node_block(ld, cur_block_);
        return ld;  // raw double bits ride in a register (JType::Unknown)
    }

    NodeId alloc_double(NodeId raw_bits) {
        const NodeId alloc =
            g_.add_aux(NodeKind::Allocate, {raw_bits}, 2, cur_region(),
                       effect_);
        chain_effect(alloc);
        set_node_block(alloc, cur_block_);
        set_type(alloc, JType::BoxedF64);
        return alloc;
    }

    NodeId make_compare(NodeId a, NodeId b, CondCode cc) {
        const NodeId c = g_.add_aux(NodeKind::Compare, {a, b},
                                    static_cast<uint32_t>(cc),
                                    cur_region());
        set_node_block(c, cur_block_);
        set_type(c, JType::Bool);
        return c;
    }

    NodeId make_const(int64_t v, JType t) {
        const NodeId c = g_.add_const(v);
        set_node_block(c, cur_block_);
        set_type(c, t);
        return c;
    }

    NodeId const_pool_load(uint32_t index) {
        // Materialized tagged pool (the J1 kCtxConstants contract): an
        // effect-free immutable load; kept effect-chained so the pool
        // pointer setup is ordered.
        const NodeId ld =
            g_.add_aux(NodeKind::Load, {},
                       static_cast<uint32_t>(AccessKind::ConstPool),
                       cur_region(), effect_);
        Node& ln = g_.node(ld);
        ln.const_value = index;
        chain_effect(ld);
        set_node_block(ld, cur_block_);
        set_type(ld, JType::Unknown);
        return ld;
    }

    NodeId load_f64_const_bits(uint32_t index) {
        const NodeId bits =
            g_.add_aux(NodeKind::Load, {},
                       static_cast<uint32_t>(AccessKind::ConstPoolF64),
                       cur_region(), effect_);
        Node& ln = g_.node(bits);
        ln.const_value = index;
        chain_effect(bits);
        set_node_block(bits, cur_block_);
        return bits;
    }

    /// The FrameState snapshot: every register's current node, aux = pc.
    /// Referenced as the LAST data input of every guard (Rule 42: complete).
    NodeId frame_state() {
        if (cur_frame_state_ != kNoNode) return cur_frame_state_;
        if (vreg_.size() != out_.register_count) {
            fprintf(stderr,
                    "[builder] frame_state with vreg_.size=%zu "
                    "register_count=%u block=%u\n",
                    vreg_.size(), out_.register_count, cur_block_);
        }
        std::vector<NodeId> inputs;
        inputs.reserve(out_.register_count);
        for (uint32_t v = 0; v < out_.register_count; ++v) {
            inputs.push_back(vreg_[v] != kNoNode
                                 ? vreg_[v]
                                 : make_const(
                                       static_cast<int64_t>(kUndefinedBits),
                                       JType::Unknown));
        }
        const NodeId fs = g_.add_aux(NodeKind::FrameState, {},
                                     cur_pc_, cur_region());
        Node& fn = g_.node(fs);
        fn.data_inputs.clear();
        for (const NodeId in : inputs) fn.data_inputs.push_back(in);
        // Frame method identity for the deopt record (Rule 42): the record
        // generator and the T0 resume both key off the OWNING method, and
        // spliced (inlined) copies keep it through the splice copy.
        fn.const_value = static_cast<int64_t>(method_.id);
        set_node_block(fs, cur_block_);
        cur_frame_state_ = fs;
        return fs;
    }

    // ---- bookkeeping --------------------------------------------------------

    void bind_branch(const ugb::Instruction& ins, NodeId cond) {
        Block& blk = out_.blocks[cur_block_];
        const NodeId iff = g_.add(NodeKind::If, {cond}, cur_region());
        set_node_block(iff, cur_block_);
        blk.control_end = iff;
        // Branch probability from the T0 profile (Rule 20/22): taken counts
        // vs total, in permille, clamped into [1, 999] so layout never
        // eliminates an edge. Unprofiled blocks keep 500.
        if (p_.profiles != nullptr &&
            cur_insn_index_ < p_.profiles->size()) {
            const ugb::ProfileSlot& prof = (*p_.profiles)[cur_insn_index_];
            const uint64_t taken = prof.branch_counts[1];
            const uint64_t not_taken = prof.branch_counts[0];
            const uint64_t total = taken + not_taken;
            if (total >= kMinBranchSamples) {
                uint64_t permille = (taken * 1000) / total;
                permille = std::clamp<uint64_t>(permille, 1, 999);
                blk.true_prob_permille = static_cast<uint32_t>(permille);
            }
        }
        blk.target_is_true_edge = ins.opcode != Op::JUMP_FALSE;
        branch_target_ = ins.meta;
    }

    void bind_return(NodeId v) {
        const NodeId ret =
            g_.add(NodeKind::Return, {v}, cur_region());
        set_node_block(ret, cur_block_);
        out_.blocks[cur_block_].control_end = ret;
        returns_.push_back(ret);
    }

    void branch_to(uint32_t target_pc) {
        const NodeId jmp = g_.add(NodeKind::End, {}, cur_region());
        set_node_block(jmp, cur_block_);
        out_.blocks[cur_block_].control_end = jmp;
        branch_target_ = target_pc;
    }

    void set_type(NodeId n, JType t) {
        if (n >= types_.size()) types_.resize(static_cast<size_t>(n) + 1,
                                              JType::Unknown);
        types_[n] = t;
    }

    JType type_of(NodeId n) const {
        return n < types_.size() ? types_[n] : JType::Unknown;
    }

    void set_node_block(NodeId n, uint32_t b) {
        if (n >= node_block_.size()) {
            node_block_.resize(static_cast<size_t>(n) + 1, UINT32_MAX);
        }
        node_block_[n] = b;
        if (n >= out_.insn_of.size()) {
            out_.insn_of.resize(static_cast<size_t>(n) + 1, UINT32_MAX);
        }
        out_.insn_of[n] = cur_insn_index_;
    }

    NodeId src(const ugb::Instruction& ins, uint32_t i) {
        return vreg_value(i < ins.srcs.size() ? ins.srcs[i] : 0);
    }

    NodeId vreg_value(uint32_t v) const {
        return v < vreg_.size() ? vreg_[v] : kNoNode;
    }

    void bind_dst(const ugb::Instruction& ins, NodeId v) {
        bind_vreg(ins.dst, v);
    }

    void bind_vreg(uint32_t v, NodeId node) {
        if (v >= vreg_.size()) vreg_.resize(static_cast<size_t>(v) + 1,
                                            kNoNode);
        vreg_[v] = node;
        cur_frame_state_ = kNoNode;  // the snapshot no longer sees the map
    }

    NodeId cur_region() const { return out_.blocks[cur_block_].region; }

    void chain_effect(NodeId n) { effect_ = n; }

    bool fail(BuildError e, uint32_t pc) {
        if (!failed_) {
            failed_ = true;
            error_ = e;
            error_pc_ = pc;
        }
        return false;
    }

    // ---- Phase D: finish ----------------------------------------------------
    void finish() {
        // Backedge phi inputs: predecessors emitted after the header fill
        // their slots now.
        for (const PendingPhi& pp : pending_phis_) {
            Block& blk = out_.blocks[pp.block];
            Node& pn = g_.node(pp.phi);
            for (size_t k = 0; k < blk.preds.size(); ++k) {
                if (k < pn.data_inputs.size() &&
                    pn.data_inputs[k] == kNoNode) {
                    pn.data_inputs[k] = pred_exit_value(blk.preds[k], pp.vreg);
                }
            }
            // Type join over the (now complete) inputs.
            JType t = JType::Unknown;
            bool first = true;
            for (const NodeId in : pn.data_inputs) {
                if (in == kNoNode) continue;
                t = first ? type_of(in) : ir::type_join(t, type_of(in));
                first = false;
            }
            set_type(pp.phi, t);
        }
        out_.entry_block = 0;
        // Pipeline inputs (stages 15/16): the speculation-dependent passes
        // (inlining, IC specialization) gate on these — a null module left
        // them permanently disabled regardless of the pass control bits.
        // The IC table feeds stage 16 (monomorphic field-access
        // specialization). The synthesized ClassGuard + RawOffset path is
        // covered by the differential parity tests (Rule 119) — a wrong
        // offset or a missing guard fails them loudly.
        out_.module = p_.module;
        out_.profiles = p_.profiles;
        out_.klass_addrs = p_.klass_addrs;
        out_.ics = p_.ics;
        out_.rpo = rpo_;
        out_.idom = idom_;
        out_.types.resize(g_.node_count(), JType::Unknown);
        for (size_t i = 0; i < types_.size() && i < out_.types.size(); ++i) {
            out_.types[i] = types_[i];
        }
        out_.block_of.resize(g_.node_count(), UINT32_MAX);
        for (size_t i = 0; i < node_block_.size() &&
                           i < out_.block_of.size(); ++i) {
            out_.block_of[i] = node_block_[i];
        }
        out_.insn_of.resize(g_.node_count(), UINT32_MAX);
        out_.osr_block_offsets.clear();
        for (const uint32_t b : backedge_sources_) {
            out_.osr_block_offsets.push_back(
                out_.blocks[b].bytecode_begin);
        }
        // Backwards compatibility for the header's Phase D contract: the
        // graphs' own dead-marking stays untouched here (DCE is a pass).
        (void)g_;
    }

    // ---- constants from the tagged-value encoding ---------------------------
    // Canonical TaggedValue words (support/tagged_value.hpp — the builder
    // mirrors the private encoding so graph constants are word-identical
    // to what T0 stores; Rule 18/39: a constant that crosses a deopt
    // boundary must be observationally identical).
    static constexpr uint64_t kNullBits = 0x3;
    static constexpr uint64_t kUndefinedBits = 0x7;
    static constexpr uint64_t kFalseBits = 0xB;
    static constexpr uint64_t kTrueBits = 0xF;
    static constexpr uint64_t kMinBranchSamples = 8;

    // ---- state ---------------------------------------------------------------
    const GraphBuilderParams& p_;
    ir::Graph& g_;
    BuiltGraph& out_;
    const ugb::UGBMethod& method_;
    BuildError error_ = BuildError::None;
    uint32_t error_pc_ = 0;
    bool failed_ = false;

    std::vector<Decoded> code_;
    std::unordered_map<uint32_t, uint32_t> insn_of_pc_;
    std::vector<uint32_t> leaders_;
    std::unordered_map<uint32_t, uint32_t> block_at_;
    std::vector<uint32_t> block_of_insn_;
    std::vector<std::pair<uint32_t, uint32_t>> block_insn_range_;
    std::vector<bool> visited_;
    std::vector<uint32_t> rpo_;
    std::vector<uint32_t> rpo_index_;
    std::vector<uint32_t> idom_;
    std::vector<std::vector<uint32_t>> df_;
    std::vector<uint32_t> backedge_sources_;  // sorted, unique
    /// Loop header -> the block that jumps back to it. The effect chain for
    /// blocks after the loop threads through the body's exit effect (see the
    /// emit_block effect computation).
    std::unordered_map<uint32_t, uint32_t> backedge_source_of_;

    std::vector<std::vector<NodeId>> block_exit_;
    std::vector<NodeId> block_exit_effect_;
    std::vector<JType> types_;
    std::vector<uint32_t> node_block_;

    struct PendingPhi {
        NodeId phi;
        uint32_t block;
        uint32_t vreg;
    };
    std::vector<PendingPhi> pending_phis_;
    std::vector<uint8_t> block_emitted_;

    std::vector<NodeId> vreg_;
    NodeId effect_ = kNoNode;
    NodeId cur_frame_state_ = kNoNode;
    uint32_t cur_block_ = 0;
    uint32_t cur_pc_ = 0;
    uint32_t cur_insn_index_ = 0;
    uint32_t branch_target_ = 0;
    std::vector<NodeId> returns_;
};

}  // namespace

BuildResult build_graph(const GraphBuilderParams& params) {
    BuildResult r;
    r.out = std::make_unique<BuiltGraph>();
    r.out->arena = std::make_unique<support::Arena>();
    r.out->graph = std::make_unique<ir::Graph>(*r.out->arena);
    Builder b(params, *r.out->graph, *r.out);
    if (b.run()) {
        r.ok = true;
    } else {
        r.ok = false;
        r.error = b.error();
        r.error_pc = b.error_pc();
        r.out.reset();  // failed builds release the graph wholesale
    }
    return r;
}

const char* build_error_message(BuildError e) noexcept {
    switch (e) {
    case BuildError::UnsupportedOpcode:
        return "J2: opcode outside the M2 fast set (stays on J1)";
    case BuildError::BudgetExceeded:
        return "J2: graph node budget exceeded (stays on J1)";
    case BuildError::MalformedBranch:
        return "J2: branch target not on an instruction boundary";
    case BuildError::DecodeError:
        return "J2: malformed instruction stream";
    case BuildError::None:
        break;
    }
    return "J2: no error";
}

}  // namespace vortex::j2
