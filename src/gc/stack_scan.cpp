// CEP:FILE: src/gc/stack_scan.cpp
// CEP:WHAT: Precise root scanning: T0 register files, JIT frames through
// the GcCodeRegistry's GC maps, handle entries, and FFI pins.
// CEP:WHY: docs/gc-icggc.md section 8.6 — Rule 78: no untracked references
// across safepoints.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: named failures ride the cycle error path; a null anchor
// yields no JIT frames.
// CEP:ASSUMES: RBP-chained JIT frames; the anchor's identity pc is the
// GC-capable helper's return address; one mutator.
// CEP:COST: @warm. O(frames + slots) per pause.
// CEP:EVIDENCE: gc_young_gc_inside_jit_allocation_keeps_parity (JIT leg),
// gc_young_evacuation_promotes_and_updates_roots (T0/handle legs).
// Precise root scanning (docs/gc-icggc.md section 8.6): T0 register files,
// JIT frames through the GcCodeRegistry's GC maps, handle entries, and FFI
// pins. Every pause (young GC and the old cycle's STW phases) walks the same
// entry point; the update passes write resolved pointers back through the
// recorded slot addresses.
//
// JIT frame walk: every published executable registers a GcCodeRecord with
// its code range and GC maps. J1 frames keep ALL vregs (every vreg slot is a
// TaggedValue; the heap-tag check makes the conservative-all map precise).
// J2/J3/J4 frames decode the published per-safepoint maps: the return
// address belongs to the nearest preceding safepoint site, each set bit is
// one frame slot in the slot_disp numbering (slot i at [rbp - 48 - 8i]).
//
// The walk follows the RBP chain (both emitters keep the push rbp; mov
// rbp,rsp prologue) and stops at the first return address outside every
// registered code range — the C++ driver boundary. C++ frames never hold
// the only reference across a GC point (docs/gc-icggc.md 8.8 invariant 1).
#include "vortex/gc/collector.hpp"

#include <bit>
#include <cstring>

#include "vortex/infra/ffi.hpp"
#include "vortex/j1/context.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/vm/interpreter.hpp"

namespace vortex::gc {

namespace {

constexpr size_t kMaxWalkFrames = 256;  // defensive chain bound

inline void stats_jit_frames(GcStats* stats) noexcept {
    if (stats != nullptr) stats->jit_frames_scanned++;
}

// CEP:WHAT: Reads one little-endian u32 from a GC-map blob.
// CEP:WHY: the J2-J4 map format is u32-streamed (Rule 86 metadata).
// CEP:STATUS: complete
// CEP:FAILURE: none (the caller bounds the stream).
// CEP:ASSUMES: pos + 4 <= b.size().
// CEP:COST: @warm. One memcpy.
// CEP:EVIDENCE: the J2-frame walk.
uint32_t read_u32(const std::vector<uint8_t>& b, size_t& pos) noexcept {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + pos, sizeof(v));
    pos += sizeof(v);
    return v;
}

// J2/J3/J4 frame slots: slot i lives at [rbp + slot_disp(i)] with
// slot_disp(i) = kJ2SpillBase - 8*i (fast_jit.hpp; the emitter's spill
// base is a public constant so the GC map walk shares one definition).
TaggedValue* j2_slot_at(uint8_t* rbp, uint32_t slot) noexcept {
    return reinterpret_cast<TaggedValue*>(rbp +
                                          (j2::kJ2SpillBase -
                                           8 * static_cast<int32_t>(slot)));
}

// CEP:WHAT: Walks the JIT frame chain, collecting GC-map-described slots.
// CEP:WHY: docs/gc-icggc.md 8.6 — the anchor frame scans first (identity pc = the helper's return address).
// CEP:STATUS: complete
// CEP:FAILURE: a lookup miss stops the walk at the C++ boundary; the chain bound is defensive.
// CEP:ASSUMES: published executables registered their records; frames are RBP-chained.
// CEP:COST: @warm per frame: registry lookup + per-slot tag test.
// CEP:EVIDENCE: gc_young_gc_inside_jit_allocation_keeps_parity.
void walk_jit_frames(uint8_t* rbp, const void* initial_pc, GcStats* stats,
                     RootSet& out) noexcept {
    if (rbp == nullptr) return;
    GcCodeRegistry& registry = gc_code_registry();
    // The ANCHOR frame is scanned first: its identity pc is the GC-capable
    // helper's return address (points INTO the anchor frame). Every later
    // frame's identity pc is its CALLEE's return address at [rbp + 8].
    const void* pc = initial_pc;
    for (size_t depth = 0; depth < kMaxWalkFrames; ++depth) {
        if (pc == nullptr) break;
        const GcCodeRecord* rec =
            registry.lookup(reinterpret_cast<uintptr_t>(pc));
        if (rec == nullptr) break;  // the C++ driver boundary
        if (rec->kind == GcCodeRecord::Kind::J1) {
            // J1: all vregs, tag-validated (precise, docs/gc-icggc.md 8.6).
            const uint32_t rc = rec->register_count;
            for (uint32_t i = 0; i < rc; ++i) {
                TaggedValue* slot = reinterpret_cast<TaggedValue*>(
                    rbp + j1::vreg_disp(rc, i));
                if (slot->is_heap_object()) out.tagged_slots.push_back(slot);
            }
        } else if (rec->gc_maps != nullptr) {
            const uintptr_t ret_off =
                reinterpret_cast<uintptr_t>(pc) -
                reinterpret_cast<uintptr_t>(rec->code_base);
            const std::vector<uint8_t>& maps = *rec->gc_maps;
            size_t pos = 0;
            const uint32_t count = read_u32(maps, pos);
            for (uint32_t e = 0; e < count; ++e) {
                const uint32_t site = read_u32(maps, pos);
                const uint32_t words = read_u32(maps, pos);
                const size_t bits_at = pos;
                pos += static_cast<size_t>(words) * 4;
                if (site >= ret_off) break;  // sites ascend; not found
                // The nearest preceding safepoint site owns this return.
                for (uint32_t w = 0; w < words; ++w) {
                    uint32_t bits = 0;
                    std::memcpy(&bits, maps.data() + bits_at + 4 * w,
                                sizeof(bits));
                    while (bits != 0) {
                        const uint32_t bit = static_cast<uint32_t>(
                            std::countr_zero(bits));
                        bits &= bits - 1;
                        TaggedValue* slot =
                            j2_slot_at(rbp, w * 32 + bit);
                        if (slot->is_heap_object()) {
                            out.tagged_slots.push_back(slot);
                        }
                    }
                }
            }
        }
        stats_jit_frames(stats);
        // Walk up: the caller's frame pointer is the saved RBP; the
        // caller's identity pc is THIS frame's return address.
        const void* next_pc = *reinterpret_cast<void* const*>(rbp + 8);
        rbp = *reinterpret_cast<uint8_t**>(rbp);  // the saved-RBP chain
        pc = next_pc;
        if (rbp == nullptr) break;
    }
}

}  // namespace

// CEP:WHAT: Collects the precise root set from the mutator state.
// CEP:WHY: every pause and UpdateRoots share this entry (docs 8.6).
// CEP:STATUS: complete
// CEP:FAILURE: named failures ride the cycle error path.
// CEP:ASSUMES: GcRuntimeState is wired before any collection.
// CEP:COST: @warm. O(frames + slots).
// CEP:EVIDENCE: all collection tests.
void scan_runtime_roots(const GcRuntimeState& state, RootSet& out,
                        GcStats* stats) {
    // 1. T0 frames: every live frame register (precise, docs/gc-icggc.md 8.6).
    if (state.interpreter != nullptr) {
        auto* interp = static_cast<vm::Interpreter*>(state.interpreter);
        interp->for_each_root_slot([&out](TaggedValue* slot) {
            out.tagged_slots.push_back(slot);
        });
    }
    // 2. JIT frames through the registry; the anchor is consumed — it is
    // only valid while the storing stencil's frame is live, and clearing it
    // keeps stale anchors from ever walking reused stack memory.
    if (state.j1ctx != nullptr) {
        auto* ctx = static_cast<j1::J1Context*>(state.j1ctx);
        walk_jit_frames(static_cast<uint8_t*>(ctx->gc_frame_rbp),
                        ctx->gc_frame_pc, stats, out);
        ctx->gc_frame_rbp = nullptr;
        ctx->gc_frame_pc = nullptr;
    }
    // 3. Handles: strong scopes are roots; weak entries are tracked for the
    // update pass but never keep objects alive (Rule 85).
    if (state.handles != nullptr) {
        auto* table = static_cast<infra::HandleTable*>(state.handles);
        table->for_each_entry([&out](uint32_t, infra::HandleTable::HandleEntry& e) {
            if (!e.alive || e.object == nullptr) return;
            if (e.scope == infra::HandleScope::Weak) {
                out.weak_slots.push_back(&e.object);
            } else {
                out.raw_slots.push_back(&e.object);
            }
        });
    }
    // 4. Pins: roots that never evacuate (docs/gc-icggc.md 8.6).
    for (void* pr : state.pin_regions) {
        auto* region = static_cast<infra::PinningRegion*>(pr);
        for (void* p : region->pinned()) {
            if (p != nullptr) out.pinned.push_back(static_cast<HeapObject*>(p));
        }
    }
}

}  // namespace vortex::gc
