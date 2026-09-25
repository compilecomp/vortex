// Infra 8 — Code-range reservation (docs/roadmap.md M2, docs/ldpt.md section 1).
//
// Production JIT runtimes reserve a large virtual-address region up front and
// carve every code arena out of it, so any two published code objects stay
// within a direct rel32 call/jmp of each other. Vortex does the same:
//
//   - J1/J2 method code (WritableCodeMemory) and LDPT patch arenas
//     (PatchArena) sub-allocate from one CodeRange;
//   - the LDPT skeleton primary path is a DIRECT `call rel32` (no
//     target-constant indirect, no workaround);
//   - OOL stubs tail-jmp their targets directly for the same reason.
//
// The reservation is PROT_NONE | MAP_NORESERVE: it costs address space only;
// pages commit through the arenas' own W^X lifecycles (mprotect + first
// touch). Reserving is @cold one-time setup; rel32 math is pure arithmetic.
//
// CEM-26: setup/infrastructure — @cold. No hot path runs in this file.
#pragma once

#include <cstdint>
#include <span>

#include "vortex/support/result.hpp"

namespace vortex::infra {

/// Largest displacement a x86-64 rel32 call/jmp can encode, signed.
inline constexpr int64_t kRel32MaxForward = 0x7FFFFFFFll;
inline constexpr int64_t kRel32MaxBackward = -0x80000000ll;

class CodeRange {
public:
    /// Reserves `bytes` (rounded to pages) of PROT_NONE address space.
    static support::Result<CodeRange> reserve(size_t bytes);

    CodeRange() noexcept = default;
    CodeRange(CodeRange&& other) noexcept;
    CodeRange& operator=(CodeRange&& other) noexcept;
    CodeRange(const CodeRange&) = delete;
    CodeRange& operator=(const CodeRange&) = delete;
    ~CodeRange();

    /// Carves a page-aligned, page-rounded sub-region and mprotects it
    /// READ|WRITE for the arena's emission phase. The caller owns the W^X
    /// lifecycle of the returned span (publish = its own mprotect to RX).
    /// Fails when the reservation is exhausted — the range size is a
    /// capacity budget, not a hint (Rule 91: code-cache pressure is managed,
    /// never silent).
    support::Result<std::span<uint8_t>> allocate(size_t bytes);

    /// Releases a previously allocated span back to the range (no unmapping:
    /// the pages return to PROT_NONE). LIFO is not required; M2 uses bump
    /// allocation and frees whole arenas only at teardown.
    void deallocate(std::span<uint8_t> region) noexcept;

    bool contains(const void* p) const noexcept;

    /// True when a direct rel32 branch from `from` can reach `to`.
    static bool within_rel32(const void* from, const void* to) noexcept;

    size_t reserved_bytes() const noexcept { return bytes_; }
    size_t used_bytes() const noexcept { return used_; }
    explicit operator bool() const noexcept { return base_ != nullptr; }

private:
    CodeRange(uint8_t* base, size_t bytes) noexcept
        : base_(base), bytes_(bytes) {}
    uint8_t* base_ = nullptr;
    size_t bytes_ = 0;
    size_t used_ = 0;  // bump cursor (M2: no reclamation in the middle)
};

/// The process-wide code range shared by the JIT tiers and the LDPT arenas.
/// Reserved lazily on first use (@cold, single-threaded init — the same
/// single-mutator contract the M0 dispatch table documents, ADR-002).
/// Explicit CodeRange* parameters exist for tests and embedding hosts that
/// bring their own reservation.
CodeRange* global_code_range() noexcept;

}  // namespace vortex::infra
