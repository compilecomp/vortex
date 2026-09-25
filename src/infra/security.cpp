#include "vortex/infra/security.hpp"

#include <sys/mman.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <random>

#include "vortex/infra/code_range.hpp"

namespace vortex::infra {

// ---------------------------------------------------------------------------
// WritableCodeMemory
// ---------------------------------------------------------------------------

// @cold — one allocation per published method.
// PERF_CONTRACT:
// BUDGET: one mmap (standalone) or one mprotect (range-carved) syscall
// READS: 0  WRITES: 0 (fresh RW mapping)
// BRANCHES: 1 (allocation failure)
// CACHE: n/a — kernel operation
support::Result<WritableCodeMemory> WritableCodeMemory::allocate(
    size_t bytes, CodeRange* range) {
    if (range != nullptr) {
        // Shared reservation: carve + flip RW (the span arrives RW already).
        auto region = range->allocate(bytes);
        if (!region) return std::unexpected(std::move(region).error());
        WritableCodeMemory m(region->data(), region->size());
        m.owner_range_ = range;
        return m;
    }
    const size_t page = 4096;
    const size_t aligned = (bytes + page - 1) & ~(page - 1);
    void* p = ::mmap(nullptr, aligned, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             std::string("code memory mmap failed: ") +
                                 std::strerror(errno));
    }
    return WritableCodeMemory(p, aligned);
}

WritableCodeMemory::WritableCodeMemory(WritableCodeMemory&& other) noexcept
    : base_(other.base_), bytes_(other.bytes_), published_(other.published_),
      owner_range_(other.owner_range_) {
    other.base_ = nullptr;
    other.bytes_ = 0;
    other.owner_range_ = nullptr;
}

WritableCodeMemory& WritableCodeMemory::operator=(
    WritableCodeMemory&& other) noexcept {
    if (this != &other) {
        if (base_ != nullptr && owner_range_ == nullptr) ::munmap(base_, bytes_);
        base_ = other.base_;
        bytes_ = other.bytes_;
        published_ = other.published_;
        owner_range_ = other.owner_range_;
        other.base_ = nullptr;
        other.bytes_ = 0;
        other.owner_range_ = nullptr;
    }
    return *this;
}

WritableCodeMemory::~WritableCodeMemory() {
    if (base_ == nullptr) return;
    if (owner_range_ != nullptr) {
        owner_range_->deallocate(std::span<uint8_t>(base_, bytes_));
        return;
    }
    ::munmap(base_, bytes_);
}

void WritableCodeMemory::write(std::span<const uint8_t> code, size_t at) {
    if (base_ == nullptr || published_) return;  // W^X: no writes after flip
    std::memcpy(base_ + at, code.data(), code.size());
}

support::Result<void> WritableCodeMemory::publish() {
    if (base_ == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "publish on empty code memory");
    }
    if (published_) return support::ok();
    // Memory barrier before the permission flip (docs/infrastructure/02).
    std::atomic_thread_fence(std::memory_order_release);
    if (::mprotect(base_, bytes_, PROT_READ | PROT_EXEC) != 0) {
        return support::fail(support::ErrorCode::InternalError,
                             std::string("mprotect to RX failed: ") +
                                 std::strerror(errno));
    }
    published_ = true;
    return support::ok();
}

// ---------------------------------------------------------------------------
// ConstantBlinding
// ---------------------------------------------------------------------------

uint64_t ConstantBlinding::random_mask() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist(1, ~0ull);
    return dist(gen);
}

bool ConstantBlinding::should_blind(int64_t value) const noexcept {
    // Strict policy: only immediates inside the small-value band are exempt
    // when a threshold is configured; everything else is blinded. With the
    // default threshold of 0, everything is blinded.
    if (threshold_ <= 0) return true;
    return value < -threshold_ || value > threshold_;
}

}  // namespace vortex::infra
