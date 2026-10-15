// Infra 5 — Native Interop (FFI) & ABI translation
// (docs/infrastructure/05-ffi.md).
//
// M0 implements the HandleTable (indirection over the moving ICGGC heap).
// Trampoline generation and unified unwinding are contract-stubbed.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "vortex/support/result.hpp"

namespace vortex::infra {

enum class HandleScope : uint8_t { Global, Weak, Local };

/// Indirection table: native code holds handles, never raw pointers into the
/// concurrently-moving heap (docs/infrastructure/05 section: object pinning &
/// handle tables).
class HandleTable {
public:
    /// One handle slot (docs/gc-icggc.md 8.6: the collector walks entries to
    /// scan/update roots and clears dead weak handles).
    struct HandleEntry {
        void* object = nullptr;
        HandleScope scope = HandleScope::Local;
        bool alive = false;
    };

    support::Result<uint32_t> create(void* object, HandleScope scope);

    /// Resolves a handle; fails for freed handles. Weak handles to objects
    /// the collector cleared resolve to a null value (named nullptr, not an
    /// error — Rule 85's valid-state boundary).
    support::Result<void*> resolve(uint32_t handle) const;

    void destroy(uint32_t handle);

    /// Frees all Local handles (scope exit).
    size_t destroy_scope(HandleScope scope);

    /// GC integration (docs/gc-icggc.md 8.6): every live entry, mutable.
    // CEP:WHAT: Walks every handle entry (including dead slots) mutably.
    // CEP:WHY: docs/gc-icggc.md 8.6 — the collector scans/updates roots and clears dead weak handles.
    // CEP:STATUS: complete
    // CEP:FAILURE: none (read/write walk).
    // CEP:ASSUMES: called only at pauses (single mutator).
    // CEP:COST: @warm. O(entries).
    // CEP:EVIDENCE: gc_compaction_preserves_identity_under_ffi_pinning (weak clearing).
    template <typename Fn>
    void for_each_entry(Fn&& fn) {
        for (uint32_t i = 0; i < entries_.size(); ++i) fn(i, entries_[i]);
    }

    size_t live_count() const noexcept { return live_; }

private:
    std::vector<HandleEntry> entries_;
    std::vector<uint32_t> free_list_;
    size_t live_ = 0;
};

/// RAII pinning region: forbids the GC from moving specific objects during a
/// native call (the GC consults registered regions — docs/gc-icggc.md 8.6:
/// pinned objects are roots, never evacuated; a pinned young object promotes
/// with its pin entry updated in place, identity preserved).
class PinningRegion {
public:
    void pin(void* object) { pinned_.push_back(object); }
    void unpin(void* object);
    bool is_pinned(const void* object) const noexcept;
    const std::vector<void*>& pinned() const noexcept { return pinned_; }
    /// Collector access: the young evacuation updates pin entries when a
    /// pinned young object is promoted (docs/gc-icggc.md 8.1).
    std::vector<void*>& pinned_mut() noexcept { return pinned_; }

private:
    std::vector<void*> pinned_;
};

/// ABI descriptors (trampoline generation is contract-stubbed, docs/roadmap.md M6).
enum class AbiKind : uint8_t { SystemVX86_64, Aapcs64 };

struct ForeignSignature {
    AbiKind abi = AbiKind::SystemVX86_64;
    uint32_t integer_args = 0;
    uint32_t float_args = 0;
    bool returns_integer = true;
};

}  // namespace vortex::infra
