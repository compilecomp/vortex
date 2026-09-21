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
    support::Result<uint32_t> create(void* object, HandleScope scope);

    /// Resolves a handle; fails for freed handles. Weak handles may resolve
    /// to nullptr after collector decisions (M5).
    support::Result<void*> resolve(uint32_t handle) const;

    void destroy(uint32_t handle);

    /// Frees all Local handles (scope exit).
    size_t destroy_scope(HandleScope scope);

    size_t live_count() const noexcept { return live_; }

private:
    struct Entry {
        void* object = nullptr;
        HandleScope scope = HandleScope::Local;
        bool alive = false;
    };
    std::vector<Entry> entries_;
    std::vector<uint32_t> free_list_;
    size_t live_ = 0;
};

/// RAII pinning region: forbids the GC from moving specific objects during a
/// native call (GC consults the active regions in M5).
class PinningRegion {
public:
    void pin(void* object) { pinned_.push_back(object); }
    void unpin(void* object);
    bool is_pinned(const void* object) const noexcept;
    const std::vector<void*>& pinned() const noexcept { return pinned_; }

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
