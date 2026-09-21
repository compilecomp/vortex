#include "vortex/ir/son_graph.hpp"

#include <vector>

namespace vortex::ir {

NodeId Graph::add(NodeKind kind, std::initializer_list<NodeId> data_inputs,
                  NodeId control, NodeId effect_in) {
    Node n;
    n.kind = kind;
    n.id = static_cast<NodeId>(nodes_.size());
    for (NodeId in : data_inputs) {
        n.data_inputs.push_back(in);
    }
    n.control = control;
    n.effect_in = effect_in;
    // Maintain the reverse effect chain: my producer's effect flows to me.
    if (effect_in != kNoNode && effect_in < nodes_.size()) {
        nodes_[effect_in].effect_outs.push_back(n.id);
    }
    nodes_.push_back(std::move(n));
    return nodes_.back().id;
}

NodeId Graph::add_const(int64_t value, NodeId control) {
    const NodeId id = add(NodeKind::Const, {}, control);
    nodes_[id].const_value = value;
    return id;
}

uint32_t Graph::eliminate_dead_nodes() {
    // Roots: Return and End (and nothing else — pure chains die by
    // unreachability, effectful chains die only when their effect is
    // unobserved). Rule 51: reachability set is a sparse set; the worklist is
    // a dense index vector.
    support::SparseSet live(static_cast<uint32_t>(nodes_.size()));
    std::vector<uint32_t> work;
    work.reserve(nodes_.size());
    for (const Node& n : nodes_) {
        if ((n.kind == NodeKind::Return || n.kind == NodeKind::End) && !n.dead) {
            live.insert(n.id);
            work.push_back(n.id);
        }
    }
    while (!work.empty()) {
        const NodeId id = work.back();
        work.pop_back();
        const Node& n = nodes_[id];
        for (NodeId in : n.data_inputs) {
            if (in != kNoNode && !live.contains(in)) {
                live.insert(in);
                work.push_back(in);
            }
        }
        if (n.control != kNoNode && !live.contains(n.control)) {
            live.insert(n.control);
            work.push_back(n.control);
        }
        if (n.effect_in != kNoNode && !live.contains(n.effect_in)) {
            live.insert(n.effect_in);
            work.push_back(n.effect_in);
        }
    }
    uint32_t killed = 0;
    for (Node& n : nodes_) {
        if (!live.contains(n.id) && !n.dead) {
            n.dead = true;
            ++killed;
        }
    }
    return killed;
}

size_t Graph::live_count() const noexcept {
    size_t n = 0;
    for (const Node& node : nodes_) n += !node.dead;
    return n;
}

}  // namespace vortex::ir
