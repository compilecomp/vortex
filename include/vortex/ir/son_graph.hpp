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
    Const, Parameter, Add, Sub, Mul, And, Or, Xor, Shift, Compare, Phi, Select,
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
};

constexpr bool is_pure(NodeKind k) noexcept {
    switch (k) {
    case NodeKind::Const: case NodeKind::Parameter: case NodeKind::Add:
    case NodeKind::Sub: case NodeKind::Mul: case NodeKind::And:
    case NodeKind::Or: case NodeKind::Xor: case NodeKind::Shift:
    case NodeKind::Compare: case NodeKind::Phi: case NodeKind::Select:
    case NodeKind::Start:
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
    int64_t const_value = 0;                      // Const payload
    bool dead = false;

    uint32_t input_count() const noexcept {
        return static_cast<uint32_t>(data_inputs.size() +
                                     (control != kNoNode ? 1u : 0u) +
                                     (effect_in != kNoNode ? 1u : 0u));
    }
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

    /// Convenience: constant node.
    NodeId add_const(int64_t value, NodeId control = kNoNode);

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
