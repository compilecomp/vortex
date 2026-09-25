// Infra 2 — Security & Exploit Mitigation System
// (docs/infrastructure/02-security.md).
//
// W^X is absolute: code memory is never simultaneously writable and executable.
// Every publication path in the runtime goes through WritableCodeMemory.
// Constant blinding defeats JIT spraying; CFI/PAC/Spectre knobs are wired into
// the codegen contracts.
#pragma once

#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <vector>

#include "vortex/support/result.hpp"

namespace vortex::infra {

class CodeRange;

// ---------------------------------------------------------------------------
// W^X memory manager. POSIX implementation: RW pages during emission, then a
// memory barrier + mprotect(PROT_READ|PROT_EXEC) flip for publication.
// ---------------------------------------------------------------------------
class WritableCodeMemory {
public:
    /// Allocates `bytes` of RW code memory (16-byte aligned). When `range`
    /// is given, the mapping is carved from that shared reservation so the
    /// published code stays within rel32 reach of every other arena in the
    /// same range (docs/ldpt.md section 1); otherwise a standalone mmap is
    /// used.
    static support::Result<WritableCodeMemory> allocate(
        size_t bytes, CodeRange* range = nullptr);

    WritableCodeMemory() noexcept = default;
    WritableCodeMemory(WritableCodeMemory&& other) noexcept;
    WritableCodeMemory& operator=(WritableCodeMemory&& other) noexcept;
    WritableCodeMemory(const WritableCodeMemory&) = delete;
    WritableCodeMemory& operator=(const WritableCodeMemory&) = delete;
    ~WritableCodeMemory();

    /// RW phase: copy code in.
    void write(std::span<const uint8_t> code, size_t at = 0);

    /// Publication: memory barrier, then flip to RX. After this call the
    /// memory must not be written again (create a new allocation to patch).
    support::Result<void> publish();

    uint8_t* data() noexcept { return base_; }
    const uint8_t* data() const noexcept { return base_; }
    size_t size() const noexcept { return bytes_; }
    bool published() const noexcept { return published_; }

private:
    WritableCodeMemory(void* base, size_t bytes) noexcept
        : base_(static_cast<uint8_t*>(base)), bytes_(bytes) {}
    uint8_t* base_ = nullptr;
    size_t bytes_ = 0;
    bool published_ = false;
    // Non-null when the mapping was carved from a shared CodeRange: the
    // destructor returns the span to the range (PROT_NONE) instead of
    // munmap, which would punch holes through the reservation.
    CodeRange* owner_range_ = nullptr;
};

// ---------------------------------------------------------------------------
// Constant blinding — XOR embedded 64-bit immediates with a per-method random
// mask and un-XOR at runtime, so attacker-controlled constants cannot become
// executable bytes in the code stream (JIT-spray prevention).
// ---------------------------------------------------------------------------
class ConstantBlinding {
public:
    static uint64_t random_mask();

    explicit ConstantBlinding(uint64_t mask) noexcept : mask_(mask) {}

    uint64_t blind(uint64_t value) const noexcept { return value ^ mask_; }
    uint64_t unblind(uint64_t value) const noexcept { return value ^ mask_; }

    /// Policy: whether small "safe" immediates are exempt (configurable; the
    /// strict default blinds everything).
    bool should_blind(int64_t value) const noexcept;
    void set_blind_threshold(int64_t threshold) noexcept { threshold_ = threshold; }

private:
    uint64_t mask_;
    int64_t threshold_ = 0;  // 0 = blind everything
};

// ---------------------------------------------------------------------------
// CFI / PAC / Spectre policy knobs (emission hooks in the codegen contracts).
// ---------------------------------------------------------------------------
struct CfiPolicy {
    bool emit_bti_arm64 = false;      // BTI on ARM64
    bool emit_ibt_x86 = false;        // Indirect Branch Tracking (CET)
    bool enable_shadow_stack = false; // software shadow stack / ARM PAC
    bool enable_pac_arm64 = false;    // Pointer Authentication
};

struct SpectrePolicy {
    bool mask_array_indices = true;   // AND-mask indices (BHI/SLP bounds)
    bool insert_lfence = false;       // LFENCE/CSDB after critical branches
    bool isolate_code_origins = true; // per-origin code cache isolation
};

}  // namespace vortex::infra
