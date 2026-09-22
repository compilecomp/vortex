// Infra 8/LDPT — Pre-flipped Patch Arenas (docs/ldpt.md section 4C).
//
// Patch trampolines are re-written at runtime, which conflicts with the W^X
// law (include/vortex/infra/security.hpp: code memory is never simultaneously
// writable and executable). A PatchArena is a dedicated code region whose
// lifecycle makes the conflict explicit and safe:
//
//   EMISSION  — pages are RW; skeletons and out-of-line stubs are assembled.
//   PUBLISH   — one flip to RX; the arena is now executable and frozen.
//   SESSION   — a bounded, explicitly-scoped RX->RW->RX round trip guarded by
//               the M:N scheduler safe-point protocol (no guest thread can be
//               executing arena code while a session is open — the caller
//               proves it by holding the handshake before begin_session).
//
// This is the mprotect-based backend. Platform alternatives (MAP_JIT on
// Apple Silicon, pthread_jit_write_protect_np, W^X via PKU) slot behind the
// same session protocol; docs/ldpt.md section 4C records the trade-offs.
//
// CEM-26: the arena is setup/patching infrastructure (@cold per patch
// session). The mprotect pair is a registered PERF_PERMIT (PERF-005,
// docs/cem26.md section 5): page flips cost microseconds and are amortized
// over a whole patch batch, never per guest call.
#pragma once

#include <cstdint>
#include <span>

#include "vortex/support/result.hpp"

namespace vortex::infra {

class PatchArena {
public:
    /// Allocates `bytes` (rounded up to whole pages) of code memory in RW
    /// state for the emission phase.
    static support::Result<PatchArena> allocate(size_t bytes);

    PatchArena() noexcept = default;
    PatchArena(PatchArena&& other) noexcept;
    PatchArena& operator=(PatchArena&& other) noexcept;
    PatchArena(const PatchArena&) = delete;
    PatchArena& operator=(const PatchArena&) = delete;
    ~PatchArena();

    /// Emission phase: copies bytes at `offset` (pages must be RW).
    support::Result<void> write(std::span<const uint8_t> bytes, size_t offset);

    /// Emission -> execution: single RW->RX flip. Required before any
    /// trampoline in this arena is callable.
    support::Result<void> publish();

    /// Opens a patch session (RX->RW). The caller MUST hold the scheduler
    /// safe point: no thread may be executing inside this arena while the
    /// session is open. Refuses when already in a session (no nesting).
    support::Result<void> begin_session();

    /// Closes a patch session (RW->RX). Symmetric with begin_session.
    support::Result<void> end_session();

    /// Session-scoped write: patches `bytes` at `offset` while the arena is
    /// writable. Fails outside a session — patching is never implicit.
    support::Result<void> patch(std::span<const uint8_t> bytes, size_t offset);

    bool published() const noexcept { return published_; }
    bool in_session() const noexcept { return in_session_; }
    size_t size() const noexcept { return bytes_; }
    size_t used_bytes() const noexcept { return used_; }

    /// Bump-allocates `bytes` (16-byte aligned) of arena space during the
    /// emission phase and returns its offset. Fails once published — the
    /// arena layout is frozen at publication; patch sessions rewrite
    /// existing bytes and never append.
    support::Result<size_t> carve(size_t bytes);

    /// Writable view of `bytes` at `offset` (emission or session state).
    uint8_t* at(size_t offset) noexcept { return base_ + offset; }
    const uint8_t* at(size_t offset) const noexcept { return base_ + offset; }

    /// Executable entry: the arena base cast to a callable address. The
    /// caller owns the cast (trampoline layouts are declared in
    /// runtime/ldpt.hpp).
    void* exec_base() const noexcept { return base_; }

private:
    PatchArena(void* base, size_t bytes) noexcept
        : base_(static_cast<uint8_t*>(base)), bytes_(bytes) {}
    support::Result<void> set_protection(int prot);

    uint8_t* base_ = nullptr;
    size_t bytes_ = 0;
    size_t used_ = 0;
    bool published_ = false;
    bool in_session_ = false;
};

}  // namespace vortex::infra
