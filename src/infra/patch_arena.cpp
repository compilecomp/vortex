#include "vortex/infra/patch_arena.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace vortex::infra {

namespace {

// Page geometry for code mappings. Named once (CEM-26 section 2); the host
// page size is the granularity of every protection flip below.
constexpr size_t kHostPageSize = 4096;
constexpr size_t kSlotAlignment = 16;

size_t round_pages(size_t bytes) noexcept {
    return (bytes + kHostPageSize - 1) & ~(kHostPageSize - 1);
}

}  // namespace

// @cold — one allocation per arena.
// PERF_CONTRACT:
// BUDGET: one mmap syscall (~10us, amortized over the arena lifetime)
// READS: 0  WRITES: 0 (fresh anonymous mapping)
// BRANCHES: 1 (mmap failure)
// CACHE: n/a — kernel operation
support::Result<PatchArena> PatchArena::allocate(size_t bytes) {
    if (bytes == 0) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "patch arena requires a non-zero size");
    }
    const size_t aligned = round_pages(bytes);
    void* p = ::mmap(nullptr, aligned, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             std::string("patch arena mmap failed: ") +
                                 std::strerror(errno));
    }
    return PatchArena(p, aligned);
}

PatchArena::PatchArena(PatchArena&& other) noexcept
    : base_(other.base_), bytes_(other.bytes_), used_(other.used_),
      published_(other.published_), in_session_(other.in_session_) {
    other.base_ = nullptr;
    other.bytes_ = 0;
}

PatchArena& PatchArena::operator=(PatchArena&& other) noexcept {
    if (this != &other) {
        if (base_ != nullptr) ::munmap(base_, bytes_);
        base_ = other.base_;
        bytes_ = other.bytes_;
        used_ = other.used_;
        published_ = other.published_;
        in_session_ = other.in_session_;
        other.base_ = nullptr;
        other.bytes_ = 0;
    }
    return *this;
}

PatchArena::~PatchArena() {
    if (base_ != nullptr) ::munmap(base_, bytes_);
}

// @cold — emission-phase copy (skeletons and stubs are assembled once).
support::Result<void> PatchArena::write(std::span<const uint8_t> bytes,
                                        size_t offset) {
    if (base_ == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "write on empty patch arena");
    }
    if (published_) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "W^X violation: emission write after publish "
                             "(use a patch session)");
    }
    if (offset + bytes.size() > bytes_) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "emission write out of arena bounds");
    }
    std::memcpy(base_ + offset, bytes.data(), bytes.size());
    return support::ok();
}

support::Result<size_t> PatchArena::carve(size_t bytes) {
    if (published_) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "arena layout is frozen after publish");
    }
    const size_t aligned =
        (used_ + kSlotAlignment - 1) & ~(kSlotAlignment - 1);
    if (aligned + bytes > bytes_) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "patch arena exhausted");
    }
    used_ = aligned + bytes;
    return aligned;
}

// @cold — publication flip. PERF_PERMIT PERF-005 covers both this and the
// session flips (docs/cem26.md): mprotect costs microseconds and is amortized
// over the whole patch batch.
support::Result<void> PatchArena::publish() {
    if (base_ == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "publish on empty patch arena");
    }
    if (published_) return support::ok();
    if (in_session_) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "publish during an open patch session");
    }
    if (auto r = set_protection(PROT_READ | PROT_EXEC); !r) return r;
    published_ = true;
    return support::ok();
}

support::Result<void> PatchArena::begin_session() {
    if (!published_) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "patch session before publish (arena is already "
                             "writable during emission)");
    }
    if (in_session_) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "patch sessions must not nest");
    }
    // The caller holds the scheduler safe point (docs/ldpt.md section 2);
    // this function is the W^X arm of the protocol and asserts the state
    // machine only.
    if (auto r = set_protection(PROT_READ | PROT_WRITE); !r) return r;
    in_session_ = true;
    return support::ok();
}

support::Result<void> PatchArena::end_session() {
    if (!in_session_) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "end_session without begin_session");
    }
    if (auto r = set_protection(PROT_READ | PROT_EXEC); !r) return r;
    in_session_ = false;
    return support::ok();
}

support::Result<void> PatchArena::patch(std::span<const uint8_t> bytes,
                                        size_t offset) {
    if (!in_session_) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "patch outside a session is a W^X violation");
    }
    if (offset + bytes.size() > bytes_) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "patch write out of arena bounds");
    }
    std::memcpy(base_ + offset, bytes.data(), bytes.size());
    return support::ok();
}

// @cold — syscall wrapper shared by publish / begin_session / end_session.
support::Result<void> PatchArena::set_protection(int prot) {
    if (::mprotect(base_, bytes_, prot) != 0) {
        return support::fail(support::ErrorCode::InternalError,
                             std::string("mprotect failed: ") +
                                 std::strerror(errno));
    }
    return support::ok();
}

}  // namespace vortex::infra
