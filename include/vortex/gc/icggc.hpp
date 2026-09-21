// ICGGC — Incremental Concurrent Generational GC (docs/gc-icggc.md).
//
// M0 implements the allocation substratum the whole stack depends on: the heap
// layout, the TLAB bump allocator, the card table, and the object model
// integration. The concurrent marking / sweeping / evacuation engine is
// contract-stubbed (docs/roadmap.md, M5).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "vortex/runtime/object_model.hpp"
#include "vortex/support/result.hpp"
#include "vortex/support/tagged_value.hpp"

namespace vortex::gc {

using support::Result;

/// Card table for cross-generational reference tracking
/// (docs/gc-icggc.md section 3, docs/tier-t0.md section 8):
///   card_table[obj >> CARD_SHIFT] = DIRTY
constexpr uint64_t kCardShift = 9;  // 512-byte cards (M0 heap granularity)
constexpr uint8_t kCardDirty = 1;
constexpr uint8_t kCardClean = 0;

class CardTable {
public:
    CardTable() noexcept = default;
    CardTable(const void* heap_base, size_t heap_bytes)
        : base_(reinterpret_cast<uint64_t>(heap_base)),
          cards_((heap_bytes >> kCardShift) + 1, kCardClean) {}

    /// Card indexing is heap-relative: (obj - heap_base) >> kCardShift.
    /// (The docs' `card_table[obj >> CARD_SHIFT]` form assumes a reserved
    /// heap range; with an allocated heap we subtract the base.)
    void mark_dirty(const void* obj) noexcept {
        const uint64_t addr = reinterpret_cast<uint64_t>(obj);
        if (addr < base_) return;
        const uint64_t idx = (addr - base_) >> kCardShift;
        if (idx < cards_.size()) cards_[idx] = kCardDirty;
    }
    uint8_t card(const void* obj) const noexcept {
        const uint64_t addr = reinterpret_cast<uint64_t>(obj);
        if (addr < base_) return kCardClean;
        const uint64_t idx = (addr - base_) >> kCardShift;
        return idx < cards_.size() ? cards_[idx] : kCardClean;
    }
    uint8_t* data() noexcept { return cards_.data(); }
    const uint8_t* data() const noexcept { return cards_.data(); }
    void clear_all() noexcept {
        for (auto& c : cards_) c = kCardClean;
    }
    size_t dirty_count() const noexcept {
        size_t n = 0;
        for (uint8_t c : cards_) n += (c == kCardDirty);
        return n;
    }

private:
    uint64_t base_ = 0;
    std::vector<uint8_t> cards_;
};

/// Thread-Local Allocation Buffer (docs/gc-icggc.md section 2). Fast path:
///   load TLAB top; add size; compare with end; bump and initialize header.
class Tlab {
public:
    explicit Tlab(uint8_t* base, size_t bytes) noexcept
        : base_(base), top_(base), end_(base + bytes) {}

    /// Bump allocation with header initialization. Returns nullptr when full.
    template <typename T>
    T* try_allocate(size_t bytes) noexcept {
        if (top_ + bytes > end_) return nullptr;
        void* p = top_;
        top_ += bytes;
        return new (p) T();  // header default-init; placement-new per object type
    }

    uint8_t* top() const noexcept { return top_; }
    uint8_t* end() const noexcept { return end_; }
    size_t remaining() const noexcept { return static_cast<size_t>(end_ - top_); }
    void reset(uint8_t* base, size_t bytes) noexcept {
        base_ = base;
        top_ = base;
        end_ = base + bytes;
    }

private:
    uint8_t* base_ = nullptr;
    uint8_t* top_ = nullptr;
    uint8_t* end_ = nullptr;
};

/// Heap statistics consumed by the pacer and the `vx stats` sink.
struct HeapStats {
    uint64_t tlab_refills = 0;
    uint64_t objects_allocated = 0;
    uint64_t bytes_allocated = 0;
    uint64_t slow_path_allocations = 0;
};

/// The young-generation heap. Object placement is 16-byte aligned
/// (a TaggedValue heap-pointer invariant, see include/vortex/support/
/// tagged_value.hpp).
class Heap {
public:
    explicit Heap(size_t young_bytes = 4 * 1024 * 1024);
    ~Heap();

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    /// Allocates a plain object with `field_count` tagged fields.
    Result<Object*> allocate_object(Klass* klass, uint32_t field_count);

    /// Allocates a boxed double.
    Result<Object*> allocate_double(double value);

    /// Allocates an array of `length` tagged elements.
    Result<ArrayObject*> allocate_array(uint32_t length);

    CardTable& card_table() noexcept { return card_table_; }
    const HeapStats& stats() const noexcept { return stats_; }
    Klass* double_klass() noexcept { return double_klass_; }

    /// Contract stub: concurrent old-generation collection
    /// (docs/roadmap.md, M5). Young collection is a stub too in M0 — the heap
    /// grows monotonically within its budget and refuses beyond it.
    Result<void> collect_young();
    Result<void> collect_old_concurrent();

private:
    Result<void> refill_tlab(size_t min_bytes);

    std::unique_ptr<uint8_t[]> young_;
    size_t young_bytes_;
    Tlab tlab_;
    CardTable card_table_;
    HeapStats stats_;
    Klass* double_klass_ = nullptr;
};

}  // namespace vortex::gc
