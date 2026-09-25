// Sea-of-Nodes IR (docs/ir-son-ciog.md section 1).
//
// Structural laws implemented here (Compiler Laws):
//   Rule 47 — the graph separates data, control, effect and guard
//             dependencies as distinct input classes.
//   Rule 48 — ALL edges are 32-bit NodeId indices; raw pointer edges are
//             forbidden in the hot optimizer. Index edges make the graph
//             compact, cache-friendly, serializable and safe across node
//             storage reallocation.
//   Rule 51 — dataflow sets (liveness/reachability) use SparseSet.
//   Rule 52 — operand lists use SmallVector inline storage.
//   Rule 67 — node storage is bulk-allocated; the graph frees wholesale
//             through the owning arena/compile phase.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "vortex/support/arena.hpp"
#include "vortex/support/containers.hpp"
#include "vortex/support/result.hpp"

namespace vortex::ir {

/// Rule 48: node references are indices, never pointers.
using NodeId = uint32_t;
inline constexpr NodeId kNoNode = 0xFFFFFFFFu;

enum class NodeKind : uint16_t {
    // pure data
    Const, Parameter, Add, Sub, Mul, Div, Rem, Neg, And, Or, Xor, Shift,
    Compare, Phi, Select,
    // raw f64 arithmetic (payload domain; inputs/outputs are double bits —
    // boxing happens through Allocate(2) + Store(DoublePayload))
    FAdd, FSub, FMul, FDiv, FNeg, IToF,
    // smi boxing boundary (the J2 untagging domain, docs/tier-j2.md section
    // 4): Untag turns a proven-Smi tagged word into its raw int63; Tag
    // re-embeds. Both are pure; canonicalization cancels Tag(Untag(x)).
    Tag, Untag,
    // effectful (effect-token chained)
    Load, Store, Call, Allocate, WriteBarrier, ReadBarrier, Safepoint,
    ExternalCall,
    // control
    If, Region, Loop, End, Return,
    // guards (produce proven values)
    TypeGuard, NullGuard, BoundsGuard, ShapeGuard, ClassGuard, OverflowGuard,
    InterfaceGuard,
    // meta
    Start,
    /// Deopt state snapshot (Rule 42: FrameState is mandatory for every
    /// guard). Data inputs = the live vreg values in register order; the
    /// aux word carries the bytecode pc. Referenced BY guard nodes through
    /// their last data input, so DCE keeps exactly the reachable states.
    FrameState,
};

/// Compare-node condition codes (aux payload of NodeKind::Compare; the
/// x64 emission maps these onto the assembler CC set).
enum class CondCode : uint8_t { Eq, Ne, LtS, LeS, GtS, GeS, LtF, LeF, GtF, GeF };

/// TypeGuard subtypes (aux payload of NodeKind::TypeGuard). The failure
/// path is always deopt to T0 (Rule 30): T0 then raises its canonical error
/// when the spec can't hold, which is observably identical to raising here.
enum class GuardKind : uint8_t {
    Smi,        // value is a smi (tag bit test)
    Heap,       // value is a heap object (tag test — null is NOT a heap obj)
    NonNull,    // heap object with a non-null klass word
    IsNull,     // value IS the null reference (CHECK_NULL)
    IsNonNull,  // value is not the null reference (CHECK_NON_NULL)
    BoxedF64,   // value is the heap's boxed-double klass
};

/// Div/Rem signedness (aux payload): the trap semantics live in the
/// emission contract (T0-verbatim checks), not in the graph.
enum class DivSignedness : uint8_t { Signed, Unsigned };

/// Call shapes (aux payload of NodeKind::Call). `Generic` is the shared
/// canonical-binop helper (aux carries the op id; the J1 generic_binop
/// contract) — distinct from `Builtin`, which invokes a guest builtin by
/// TOKEN through the args-window invoke ABI. Conflating the two would make
/// the backend read the wrong argument register contract.
enum class CallShape : uint8_t { Direct, Virtual, Builtin, Generic };

/// Load/Store access kinds (aux payload). ConstPool/ConstPoolF64 carry the
/// pool index in Node::const_value; Field/ArrayElement carry the field token
/// there as well (discriminated by aux — no magic reinterpretations).
enum class AccessKind : uint8_t {
    Field, ArrayElement, ArrayLength, DoublePayload, ConstPool, ConstPoolF64,
    /// Field with a proven byte offset (IC-specialized; the offset rides
    /// const_value). The class guard upstream proves the layout.
    RawOffset,
};

constexpr bool is_pure(NodeKind k) noexcept {
    switch (k) {
    case NodeKind::Const: case NodeKind::Parameter: case NodeKind::Add:
    case NodeKind::Sub: case NodeKind::Mul: case NodeKind::Div:
    case NodeKind::Rem: case NodeKind::Neg: case NodeKind::And:
    case NodeKind::Or: case NodeKind::Xor: case NodeKind::Shift:
    case NodeKind::Compare: case NodeKind::Phi: case NodeKind::Select:
    case NodeKind::Tag: case NodeKind::Untag: case NodeKind::Start:
    case NodeKind::FAdd: case NodeKind::FSub: case NodeKind::FMul:
    case NodeKind::FDiv: case NodeKind::FNeg: case NodeKind::IToF:
        return true;
    default:
        return false;
    }
}

constexpr bool is_guard(NodeKind k) noexcept {
    return k >= NodeKind::TypeGuard && k <= NodeKind::InterfaceGuard;
}

constexpr bool is_effectful(NodeKind k) noexcept {
    switch (k) {
    case NodeKind::Load: case NodeKind::Store: case NodeKind::Call:
    case NodeKind::Allocate: case NodeKind::WriteBarrier:
    case NodeKind::ReadBarrier: case NodeKind::Safepoint:
    case NodeKind::ExternalCall:
        return true;
    default:
        return false;
    }
}

/// A graph node. Inputs are grouped by dependency class:
///   data    : value inputs (0..)
///   control : control input (usually 1)
///   effect  : effect input + reverse edges (consumers of my effect)
/// All edges are NodeId indices (Rule 48).
struct Node {
    NodeKind kind = NodeKind::Start;
    NodeId id = kNoNode;
    support::SmallVector<NodeId, 4> data_inputs;
    NodeId control = kNoNode;
    NodeId effect_in = kNoNode;
    support::SmallVector<NodeId, 2> effect_outs;  // reverse edges
    int64_t const_value = 0;                      // Const payload (int bits
                                                  // or f64 bit pattern)
    /// Kind-specific payload: CondCode for Compare, GuardKind for TypeGuard,
    /// CallShape for Call, AccessKind for Load/Store, bytecode pc for
    /// FrameState, field/array token for access nodes. Named per kind — no
    /// magic reinterpretations (CEM-26 section 2).
    uint32_t aux = 0;
    bool dead = false;

    uint32_t input_count() const noexcept {
        return static_cast<uint32_t>(data_inputs.size() +
                                     (control != kNoNode ? 1u : 0u) +
                                     (effect_in != kNoNode ? 1u : 0u));
    }
    // Layout audit (CEM-26 section 9): kind/id/aux words + SmallVector(16) +
    // NodeId(4) + i64 + bool — asserted below against silent growth.
};

/// The Sea-of-Nodes graph. Nodes live in one contiguous store; ids are dense
/// indices. Reallocation of the store cannot dangle edges because every edge
/// is an index (Rule 48).
class Graph {
public:
    explicit Graph(support::Arena& arena) : arena_(arena) {}

    /// Adds a node; returns its dense NodeId.
    NodeId add(NodeKind kind,
               std::initializer_list<NodeId> data_inputs = {},
               NodeId control = kNoNode,
               NodeId effect_in = kNoNode);

    /// Dynamic-input variant (pipeline splices copy nodes with remapped
    /// edge lists).
    NodeId add_vec(NodeKind kind, std::vector<NodeId> data_inputs,
                   NodeId control = kNoNode, NodeId effect_in = kNoNode);

    /// Convenience: constant node.
    NodeId add_const(int64_t value, NodeId control = kNoNode);

    /// Convenience: const + aux in one step (access tokens, compare codes).
    NodeId add_aux(NodeKind kind, std::initializer_list<NodeId> data_inputs,
                   uint32_t aux, NodeId control = kNoNode,
                   NodeId effect_in = kNoNode);

    Node& node(NodeId id) noexcept { return nodes_[id]; }
    const Node& node(NodeId id) const noexcept { return nodes_[id]; }
    bool valid(NodeId id) const noexcept { return id < nodes_.size(); }

    /// Reachability-based DCE (docs/ir-son-ciog.md 5.3): marks nodes dead when
    /// they cannot be reached from Return/End through data, control, effect,
    /// and guard dependencies. Returns the number of newly dead nodes.
    uint32_t eliminate_dead_nodes();

    std::span<const Node> nodes() const noexcept { return {nodes_.data(), nodes_.size()}; }
    size_t node_count() const noexcept { return nodes_.size(); }
    size_t live_count() const noexcept;

private:
    support::Arena& arena_;          // auxiliary per-graph storage (Rule 67)
    std::vector<Node> nodes_;        // dense; NodeId == index
};

}  // namespace vortex::ir
