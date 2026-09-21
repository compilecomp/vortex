#include "vortex/ir/son_graph.hpp"

#include <algorithm>
#include <deque>

namespace vortex::ir {

Node* Graph::add(NodeKind kind, std::initializer_list<Node*> data_inputs,
                 Node* control, Node* effect_in) {
    Node* n = arena_.make<Node>();
    n->kind = kind;
    n->id = static_cast<uint32_t>(nodes_.size());
    for (Node* in : data_inputs) {
        n->data_inputs.push_back(in);
    }
    n->control = control;
    n->effect_in = effect_in;
    // Maintain the reverse effect chain: my producer's effect flows to me.
    if (effect_in != nullptr) {
        effect_in->effect_outs.push_back(n);
    }
    nodes_.push_back(n);
    return n;
}

uint32_t Graph::eliminate_dead_nodes() {
    // Roots: Return and End (and nothing else — pure chains die by unreachability,
    // effectful chains die only when their effect is unobserved).
    std::deque<Node*> work;
    for (Node* n : nodes_) {
        if (n->kind == NodeKind::Return || n->kind == NodeKind::End) {
            if (!n->dead) {
                n->dead = false;
                work.push_back(n);
            }
        }
    }
    std::vector<uint8_t> live(nodes_.size(), 0);
    while (!work.empty()) {
        Node* n = work.front();
        work.pop_front();
        if (live[n->id]) continue;
        live[n->id] = 1;
        for (Node* in : n->data_inputs) {
            if (in && !live[in->id]) work.push_back(in);
        }
        if (n->control && !live[n->control->id]) work.push_back(n->control);
        if (n->effect_in && !live[n->effect_in->id]) work.push_back(n->effect_in);
    }
    uint32_t killed = 0;
    for (Node* n : nodes_) {
        if (!live[n->id] && !n->dead) {
            n->dead = true;
            ++killed;
        }
    }
    return killed;
}

size_t Graph::live_count() const noexcept {
    size_t n = 0;
    for (const Node* node : nodes_) n += !node->dead;
    return n;
}

}  // namespace vortex::ir
