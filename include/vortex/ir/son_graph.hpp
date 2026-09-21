// Sea-of-Nodes IR (docs/ir-son-ciog.md section 1). M0 implements the graph
// substrate: node kinds across the four dependency classes (data, control,
// effect, guard), arena allocation, input wiring, and reachability-based dead
// node elimination. The full pass pipeline rides on this substrate in M2/M3
// (docs/roadmap.md).
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "vortex/support/arena.hpp"
#include "vortex/support/result.hpp"

namespace vortex::ir {

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
///   effect  : effect input + output token (effectful nodes)
///   guard   : guard dependency (guards only)
struct Node {
    NodeKind kind = NodeKind::Start;
    uint32_t id = 0;
    std::vector<Node*> data_inputs;
    Node* control = nullptr;
    Node* effect_in = nullptr;
    std::vector<Node*> effect_outs;  // reverse edges: consumers of my effect
    int64_t const_value = 0;         // Const payload
    bool dead = false;

    uint32_t input_count() const noexcept {
        return static_cast<uint32_t>(data_inputs.size() + (control ? 1 : 0) +
                                     (effect_in ? 1 : 0));
    }
};

/// The Sea-of-Nodes graph. Nodes are arena-allocated; ids are dense.
class Graph {
public:
    explicit Graph(support::Arena& arena) : arena_(arena) {}

    Node* add(NodeKind kind, std::initializer_list<Node*> data_inputs = {},
              Node* control = nullptr, Node* effect_in = nullptr);

    /// Reachability-based DCE (docs/ir-son-ciog.md 5.3): marks nodes dead when
    /// they cannot be reached from Return/End through data, control, effect,
    /// and guard dependencies. Returns the number of newly dead nodes.
    uint32_t eliminate_dead_nodes();

    std::span<Node* const> nodes() const noexcept { return nodes_; }
    size_t live_count() const noexcept;

private:
    support::Arena& arena_;
    std::vector<Node*> nodes_;
};

}  // namespace vortex::ir
