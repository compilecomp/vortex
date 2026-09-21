// Region-style arena allocator used by every compiler component. Compilation is
// a phase: all IR, metadata and stencil scratch memory for one compile job comes
// from one arena and is freed wholesale (docs/ir-son-ciog.md, docs/tier-j1.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

namespace vortex::support {

class Arena {
public:
    explicit Arena(size_t chunk_bytes = 64 * 1024) : chunk_bytes_(chunk_bytes) {}
    ~Arena() { reset(); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
        bytes = (bytes + alignment - 1) & ~(alignment - 1);
        if (bytes == 0) bytes = 1;
        if (current_ != nullptr) {
            uint8_t* p = align_up(current_ + used_, alignment);
            if (p + bytes <= current_ + chunk_bytes_) {
                used_ = static_cast<size_t>(p - current_) + bytes;
                return p;
            }
        }
        return allocate_new_chunk(bytes, alignment);
    }

    template <typename T, typename... Args>
    T* make(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    template <typename T>
    T* make_array(size_t count) {
        void* mem = allocate(sizeof(T) * count, alignof(T));
        T* base = static_cast<T*>(mem);
        for (size_t i = 0; i < count; ++i) new (base + i) T();
        return base;
    }

    /// Frees every chunk. Pointers into the arena are invalidated.
    void reset() {
        for (uint8_t* chunk : chunks_) operator delete[](chunk);
        chunks_.clear();
        current_ = nullptr;
        used_ = 0;
        total_bytes_ = 0;
    }

    size_t total_bytes() const noexcept { return total_bytes_; }

private:
    static uint8_t* align_up(uint8_t* p, size_t alignment) {
        return reinterpret_cast<uint8_t*>(
            (reinterpret_cast<uintptr_t>(p) + alignment - 1) &
            ~(static_cast<uintptr_t>(alignment) - 1));
    }

    uint8_t* allocate_new_chunk(size_t bytes, size_t alignment) {
        size_t chunk = chunk_bytes_ > bytes ? chunk_bytes_ : bytes;
        uint8_t* mem = static_cast<uint8_t*>(operator new[](chunk));
        chunks_.push_back(mem);
        current_ = mem;
        used_ = bytes;  // first allocation consumes from offset 0
        total_bytes_ += chunk;
        // align_up(mem) must land inside the chunk for the fast path later;
        // `operator new[]` returns memory suitably aligned for max_align_t,
        // and we bumped `used_` past the payload already.
        (void)alignment;
        return mem;
    }

    size_t chunk_bytes_;
    std::vector<uint8_t*> chunks_;
    uint8_t* current_ = nullptr;
    size_t used_ = 0;
    size_t total_bytes_ = 0;
};

}  // namespace vortex::support
