#include "vortex/infra/code_range.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace vortex::infra {

namespace {

constexpr size_t kHostPageSize = 4096;

size_t round_pages(size_t bytes) noexcept {
    return (bytes + kHostPageSize - 1) & ~(kHostPageSize - 1);
}

}  // namespace

// @cold — one-time address-space reservation.
// PERF_CONTRACT:
// BUDGET: one mmap syscall (~10us) for the whole process lifetime
// READS: 0  WRITES: 0 (PROT_NONE reservation; no pages touched)
// BRANCHES: 1 (mmap failure)
// CACHE: n/a — kernel operation
support::Result<CodeRange> CodeRange::reserve(size_t bytes) {
    if (bytes == 0) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "code range requires a non-zero size");
    }
    const size_t aligned = round_pages(bytes);
    void* p = ::mmap(nullptr, aligned, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             std::string("code range mmap failed: ") +
                                 std::strerror(errno));
    }
    return CodeRange(static_cast<uint8_t*>(p), aligned);
}

CodeRange::CodeRange(CodeRange&& other) noexcept
    : base_(other.base_), bytes_(other.bytes_), used_(other.used_) {
    other.base_ = nullptr;
    other.bytes_ = 0;
    other.used_ = 0;
}

CodeRange& CodeRange::operator=(CodeRange&& other) noexcept {
    if (this != &other) {
        if (base_ != nullptr) ::munmap(base_, bytes_);
        base_ = other.base_;
        bytes_ = other.bytes_;
        used_ = other.used_;
        other.base_ = nullptr;
        other.bytes_ = 0;
        other.used_ = 0;
    }
    return *this;
}

CodeRange::~CodeRange() {
    if (base_ != nullptr) ::munmap(base_, bytes_);
}

// @cold — bump carve + one mprotect to RW (PERF-005 class; per-arena, not
// per guest event).
support::Result<std::span<uint8_t>> CodeRange::allocate(size_t bytes) {
    if (base_ == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "allocate on empty code range");
    }
    const size_t aligned = round_pages(bytes);
    const size_t cursor = (used_ + kHostPageSize - 1) & ~(kHostPageSize - 1);
    if (cursor + aligned > bytes_) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "code range exhausted (Rule 91: capacity is a "
                             "budget — grow the reservation or evict)");
    }
    uint8_t* region = base_ + cursor;
    if (::mprotect(region, aligned, PROT_READ | PROT_WRITE) != 0) {
        return support::fail(support::ErrorCode::InternalError,
                             std::string("code range mprotect failed: ") +
                                 std::strerror(errno));
    }
    used_ = cursor + aligned;
    return std::span<uint8_t>(region, aligned);
}

void CodeRange::deallocate(std::span<uint8_t> region) noexcept {
    if (base_ == nullptr || region.data() == nullptr) return;
    // The span must be page-aligned (it came from allocate); returning it to
    // PROT_NONE makes future use a hard fault instead of silent corruption.
    (void)::mprotect(region.data(), region.size(), PROT_NONE);
}

bool CodeRange::contains(const void* p) const noexcept {
    if (base_ == nullptr || p == nullptr) return false;
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= reinterpret_cast<uintptr_t>(base_) &&
           v < reinterpret_cast<uintptr_t>(base_) + bytes_;
}

bool CodeRange::within_rel32(const void* from, const void* to) noexcept {
    const int64_t delta = static_cast<int64_t>(reinterpret_cast<uintptr_t>(to)) -
                          static_cast<int64_t>(reinterpret_cast<uintptr_t>(from));
    return delta >= kRel32MaxBackward && delta <= kRel32MaxForward;
}

// @cold — lazy process-wide reservation. Single-mutator init (ADR-002
// contract): first caller reserves; later callers observe the pointer.
CodeRange* global_code_range() noexcept {
    static CodeRange range = [] {
        // 1 GiB of address space: enough for the M2 tier stack + LDPT
        // arenas under test and benchmark loads; MAP_NORESERVE keeps the
        // reservation free of physical cost (Rule 72: named, documented).
        constexpr size_t kDefaultCodeRangeBytes = 1ull << 30;
        auto reserved = CodeRange::reserve(kDefaultCodeRangeBytes);
        return reserved ? std::move(*reserved) : CodeRange();
    }();
    return &range;
}

}  // namespace vortex::infra
