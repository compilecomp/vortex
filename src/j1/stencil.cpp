#include "vortex/j1/stencil.hpp"

namespace vortex::j1 {

void StencilTable::register_stencil(Stencil s) {
    stencils_.push_back(std::move(s));
}

void StencilTable::register_superstencil(Superstencil s) {
    superstencils_.push_back(std::move(s));
}

const Stencil* StencilTable::select(uint16_t opcode, bool profile_stable) const {
    const Stencil* generic = nullptr;
    for (const Stencil& s : stencils_) {
        if (s.opcode != opcode) continue;
        if (s.speculative) {
            if (profile_stable) return &s;  // typed variant wins when stable
        } else {
            generic = &s;  // remember generic fallback
        }
    }
    return generic;
}

const Superstencil* StencilTable::match_superstencil(const uint16_t* code,
                                                     size_t max_ops) const {
    const Superstencil* best = nullptr;
    for (const Superstencil& s : superstencils_) {
        if (s.sequence.size() > max_ops) continue;
        bool match = true;
        for (size_t i = 0; i < s.sequence.size(); ++i) {
            if (code[i] != s.sequence[i]) {
                match = false;
                break;
            }
        }
        if (match && (best == nullptr || s.sequence.size() > best->sequence.size())) {
            best = &s;  // longest match wins
        }
    }
    return best;
}

}  // namespace vortex::j1
