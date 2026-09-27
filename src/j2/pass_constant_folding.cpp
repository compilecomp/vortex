// Stages 3+4: constant folding. Refuses to fold operations whose T0
// semantics TRAP on the constant inputs (Rules 107/110).
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

// ---- stage 3+4: constant folding ---------------------------------------------------

bool smi_const(const Node& c) {
    if (c.const_value & 1) return false;
    const int64_t raw = c.const_value >> 1;
    return raw >= smi_min() && raw <= smi_max();
}

double bits_to_f64(int64_t bits) {
    double d = 0.0;
    __builtin_memcpy(&d, &bits, sizeof(d));
    return d;
}

int64_t f64_to_bits(double d) {
    int64_t bits = 0;
    __builtin_memcpy(&bits, &d, sizeof(bits));
    return bits;
}

bool fold_one(ir::Graph& g, BuiltGraph& built, Node& node) {
    if (node.data_inputs.empty()) return false;
    for (const NodeId in : node.data_inputs) {
        if (g.node(in).kind != NodeKind::Const) return false;
    }
    const Node& lhs = g.node(node.data_inputs[0]);
    auto untag = [](const Node& c) { return c.const_value >> 1; };
    auto finish = [&](int64_t tagged, JType t) {
        const NodeId c = clone_const(g, built, node.id, tagged, t);
        replace_uses(g, node.id, c);
        return true;
    };
    switch (node.kind) {
    case NodeKind::Add: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        if (!smi_const(lhs) || !smi_const(rhs)) return false;
        int64_t r = 0;
        if (__builtin_add_overflow(untag(lhs), untag(rhs), &r)) return false;
        if (r < smi_min() || r > smi_max()) return false;  // would trap
        return finish(r << 1, JType::Smi);
    }
    case NodeKind::Sub: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        if (!smi_const(lhs) || !smi_const(rhs)) return false;
        int64_t r = 0;
        if (__builtin_sub_overflow(untag(lhs), untag(rhs), &r)) return false;
        if (r < smi_min() || r > smi_max()) return false;
        return finish(r << 1, JType::Smi);
    }
    case NodeKind::Mul: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        if (!smi_const(lhs) || !smi_const(rhs)) return false;
        int64_t r = 0;
        if (__builtin_mul_overflow(untag(lhs), untag(rhs), &r)) return false;
        if (r < smi_min() || r > smi_max()) return false;
        return finish(r << 1, JType::Smi);
    }
    case NodeKind::And: case NodeKind::Or: case NodeKind::Xor: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        if (!smi_const(lhs) || !smi_const(rhs)) return false;
        int64_t r = 0;
        switch (node.kind) {
        case NodeKind::And: r = lhs.const_value & rhs.const_value; break;
        case NodeKind::Or: r = lhs.const_value | rhs.const_value; break;
        default: r = lhs.const_value ^ rhs.const_value; break;
        }
        return finish(r, JType::Smi);
    }
    case NodeKind::Compare: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        bool r = false;
        switch (static_cast<CondCode>(node.aux)) {
        case CondCode::Eq: {
            if (lhs.const_value != rhs.const_value) return false;
            r = true;
            break;
        }
        case CondCode::Ne: {
            if (lhs.const_value != rhs.const_value) return false;
            r = false;
            break;
        }
        case CondCode::LtS: case CondCode::LeS: case CondCode::GtS:
        case CondCode::GeS: {
            if (!smi_const(lhs) || !smi_const(rhs)) return false;
            const int64_t x = untag(lhs), y = untag(rhs);
            switch (static_cast<CondCode>(node.aux)) {
            case CondCode::LtS: r = x < y; break;
            case CondCode::LeS: r = x <= y; break;
            case CondCode::GtS: r = x > y; break;
            default: r = x >= y; break;
            }
            break;
        }
        case CondCode::LtF: case CondCode::LeF: case CondCode::GtF:
        case CondCode::GeF: {
            const double x = bits_to_f64(lhs.const_value);
            const double y = bits_to_f64(rhs.const_value);
            switch (static_cast<CondCode>(node.aux)) {
            case CondCode::LtF: r = x < y; break;
            case CondCode::LeF: r = x <= y; break;
            case CondCode::GtF: r = x > y; break;
            default: r = x >= y; break;
            }
            break;
        }
        default:
            return false;
        }
        // Canonical boolean words (false = 0xB, true = 0xF — the same
        // encoding TaggedValue::boolean stores; a folded compare that
        // returned smi 0/1 would diverge from T0 on every
        // boolean-identity consumer, Rule 18/39).
        constexpr uint64_t kFoldTrueBits = 0xF;
        constexpr uint64_t kFoldFalseBits = 0xB;
        return finish(r ? kFoldTrueBits : kFoldFalseBits, JType::Bool);
    }
    case NodeKind::FAdd: case NodeKind::FSub: case NodeKind::FMul:
    case NodeKind::FDiv: {
        if (node.data_inputs.size() != 2) return false;
        const Node& rhs = g.node(node.data_inputs[1]);
        const double x = bits_to_f64(lhs.const_value);
        const double y = bits_to_f64(rhs.const_value);
        double r = 0.0;
        switch (node.kind) {
        case NodeKind::FAdd: r = x + y; break;
        case NodeKind::FSub: r = x - y; break;
        case NodeKind::FMul: r = x * y; break;
        default: r = x / y; break;  // IEEE div-zero: inf/nan (Rule 110)
        }
        return finish(f64_to_bits(r), JType::Unknown);
    }
    default:
        return false;
    }
}

uint32_t fold_constants(ir::Graph& g, BuiltGraph& built) {
    // Named (Rule 72): sweep rounds until fixpoint — enough for chained
    // folds (a folded input enables a parent's fold) at J2 graph sizes.
    constexpr int kFoldSweeps = 4;
    uint32_t folded = 0;
    for (int sweep = 0; sweep < kFoldSweeps; ++sweep) {
        bool changed = false;
        for (uint32_t id = 0; id < g.node_count(); ++id) {
            Node& node = g.node(id);
            if (node.dead || !is_pure_kind(node.kind)) continue;
            if (node.kind == NodeKind::Const ||
                node.kind == NodeKind::Parameter) {
                continue;
            }
            if (fold_one(g, built, node)) {
                node.dead = true;
                ++folded;
                changed = true;
            }
        }
        if (!changed) break;
    }
    return folded;
}

}  // namespace vortex::j2
