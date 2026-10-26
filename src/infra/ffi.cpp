// CEP:FILE: src/infra/ffi.cpp
// CEP:WHAT: Infra 5 handle-table and pinning-region implementation (M0
//           surface, docs/infrastructure/05-ffi.md): scope-checked global/
//           weak/local handles over the moving ICGGC heap and the RAII pin
//           scopes the collector consults (docs/gc-icggc.md 8.6).
// CEP:WHY: Native code must never hold raw pointers into the concurrently
//           moving heap — the handle indirection and the pin registry are
//           the two sanctioned channels across the FFI boundary.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Handle resolution of dead/out-of-range slots is a named
//           InvalidArgument reject; destroy of an unknown id is a no-op;
//           unpin of an unregistered object is a no-op. Never raises.
// CEP:ASSUMES: Called only at pauses or from the single mutator (M0
//           single-thread contract — the table is not locked); the
//           collector walks entries via for_each_entry at pauses.
// CEP:COST: @warm handle ops are O(1) amortized (free list); scope destroy
//           and pin/unpin are O(entries)/O(pins) linear scans (handfuls).
// CEP:EVIDENCE: tests/test_ffi.cpp + the GC pinning suite
//           (gc_compaction_preserves_identity_under_ffi_pinning).
#include "vortex/infra/ffi.hpp"

#include <utility>

namespace vortex::infra {

// CEP:WHAT: Allocates a handle slot (free-list reuse first) for an object in a scope.
// CEP:WHY: The M0 handle/pin contract (module header).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — see module CEP:FAILURE.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @warm. O(1) amortized; one vector slot on growth.
// CEP:EVIDENCE: tests/test_ffi.cpp + the GC pinning suite.
support::Result<uint32_t> HandleTable::create(void* object, HandleScope scope) {
    uint32_t id;
    if (!free_list_.empty()) {
        id = free_list_.back();
        free_list_.pop_back();
    } else {
        id = static_cast<uint32_t>(entries_.size());
        entries_.emplace_back();
    }
    entries_[id] = HandleEntry{object, scope, true};
    ++live_;
    return id;
}

// CEP:WHAT: Resolves a live handle; dead/out-of-range slots reject named.
// CEP:WHY: The M0 handle/pin contract (module header).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — see module CEP:FAILURE.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @warm. O(1).
// CEP:EVIDENCE: tests/test_ffi.cpp + the GC pinning suite.
support::Result<void*> HandleTable::resolve(uint32_t handle) const {
    if (handle >= entries_.size() || !entries_[handle].alive) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "resolve: dead or out-of-range handle " +
                                 std::to_string(handle));
    }
    return entries_[handle].object;
}

// CEP:WHAT: Kills a handle slot and recycles it onto the free list.
// CEP:WHY: The M0 handle/pin contract (module header).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — see module CEP:FAILURE.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @warm. O(1).
// CEP:EVIDENCE: tests/test_ffi.cpp + the GC pinning suite.
void HandleTable::destroy(uint32_t handle) {
    if (handle < entries_.size() && entries_[handle].alive) {
        entries_[handle].alive = false;
        entries_[handle].object = nullptr;
        free_list_.push_back(handle);
        --live_;
    }
}

// CEP:WHAT: Frees every live handle in a scope (scope exit).
// CEP:WHY: The M0 handle/pin contract (module header).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — see module CEP:FAILURE.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold-ish. O(entries) linear scan.
// CEP:EVIDENCE: tests/test_ffi.cpp + the GC pinning suite.
size_t HandleTable::destroy_scope(HandleScope scope) {
    size_t freed = 0;
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].alive && entries_[i].scope == scope) {
            destroy(i);
            ++freed;
        }
    }
    return freed;
}

// CEP:WHAT: Removes one pin (first match) from the region.
// CEP:WHY: The M0 handle/pin contract (module header).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — see module CEP:FAILURE.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @warm. O(pins) linear scan over handfuls.
// CEP:EVIDENCE: tests/test_ffi.cpp + the GC pinning suite.
void PinningRegion::unpin(void* object) {
    for (auto it = pinned_.begin(); it != pinned_.end(); ++it) {
        if (*it == object) {
            pinned_.erase(it);
            return;
        }
    }
}

// CEP:WHAT: Containment check the collector consults before evacuation.
// CEP:WHY: The M0 handle/pin contract (module header).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — see module CEP:FAILURE.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @warm. O(pins) linear scan.
// CEP:EVIDENCE: tests/test_ffi.cpp + the GC pinning suite.
bool PinningRegion::is_pinned(const void* object) const noexcept {
    for (const void* p : pinned_) {
        if (p == object) return true;
    }
    return false;
}

}  // namespace vortex::infra
