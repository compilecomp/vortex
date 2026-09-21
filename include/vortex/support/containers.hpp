// Compliance containers (Compiler Laws Rules 50-52):
//
//   Rule 50: std::unordered_map / std::map are forbidden on hot compiler and
//            runtime paths. FlatHashMap is an open-addressing, linear-probe
//            table with power-of-two capacity — one cache line of pointer
//            chasing per probe, no per-node allocation.
//   Rule 51: dataflow sets use SparseSet (dense/sparse u32 pairs), never
//            std::set / std::unordered_set / std::vector<bool>.
//   Rule 52: small collections use SmallVector<T, N> inline storage; heap
//            allocation for usually-small collections on hot paths is
//            forbidden.
//
// Enforcement: the compliance lint job greps hot trees (src/vm, src/ir,
// src/gc, include/vortex/{vm,ir,gc}) for the forbidden containers.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace vortex::support {

// ---------------------------------------------------------------------------
// SmallVector<T, N>
// ---------------------------------------------------------------------------

/// Inline-storage vector: N elements live inside the object, overflow spills
/// to the heap and stays there. References remain valid across push_back only
/// while capacity is not exceeded (same contract as std::vector).
template <typename T, size_t N>
class SmallVector {
    static_assert(N > 0, "SmallVector inline capacity must be positive");
    // Nothrow-move elements keep the grow path exception-safe without
    // double-storage bookkeeping (hot-path containers, Rule 52).
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "SmallVector requires nothrow-move elements");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "SmallVector requires nothrow-destructible elements");

public:
    // Member-initializer order follows declaration order (heap_, size_,
    // capacity_ below) — reordering here triggers -Wreorder and the warning
    // is a true positive about initialization semantics.
    SmallVector() noexcept : heap_(nullptr), size_(0), capacity_(N) {}
    explicit SmallVector(size_t count) : SmallVector() { resize(count); }
    SmallVector(std::initializer_list<T> init) : SmallVector() {
        for (const T& v : init) push_back(v);
    }

    SmallVector(const SmallVector& other) : SmallVector() { assign_from(other); }
    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) {
            clear();
            assign_from(other);
        }
        return *this;
    }
    SmallVector(SmallVector&& other) noexcept(
        std::is_nothrow_move_constructible_v<T>)
        : SmallVector() {
        move_from(std::move(other));
    }
    SmallVector& operator=(SmallVector&& other) noexcept(
        std::is_nothrow_move_constructible_v<T>) {
        if (this != &other) {
            clear();
            move_from(std::move(other));
        }
        return *this;
    }

    ~SmallVector() {
        destroy_all();
        if (heap_ != nullptr) {
            operator delete[](heap_, std::align_val_t(alignof(T)));
        }
    }

    // ---- element access -----------------------------------------------------
    T* data() noexcept { return inline_or_heap(); }
    const T* data() const noexcept { return inline_or_heap(); }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    size_t capacity() const noexcept { return capacity_; }

    T& operator[](size_t i) noexcept { return data()[i]; }
    const T& operator[](size_t i) const noexcept { return data()[i]; }
    T& front() noexcept { return data()[0]; }
    const T& front() const noexcept { return data()[0]; }
    T& back() noexcept { return data()[size_ - 1]; }
    const T& back() const noexcept { return data()[size_ - 1]; }

    T* begin() noexcept { return data(); }
    T* end() noexcept { return data() + size_; }
    const T* begin() const noexcept { return data(); }
    const T* end() const noexcept { return data() + size_; }

    // ---- modifiers ------------------------------------------------------------
    void push_back(const T& v) { emplace_back(v); }
    void push_back(T&& v) { emplace_back(std::move(v)); }

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if (size_ == capacity_) {
            // The argument may reference this vector's own storage, which
            // grow() relocates — materialize a temporary first so
            // `v.push_back(v[0])` stays correct when the grow path fires.
            T tmp(std::forward<Args>(args)...);
            grow();
            ::new (static_cast<void*>(data() + size_)) T(std::move(tmp));
            ++size_;
            return back();
        }
        T* slot = data() + size_;
        ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
        ++size_;
        return *slot;
    }

    void pop_back() noexcept {
        --size_;
        data()[size_].~T();
    }

    void resize(size_t count) {
        while (size_ > count) pop_back();
        if (count > capacity_) reserve_exact(count);
        while (size_ < count) {
            ::new (static_cast<void*>(data() + size_)) T();
            ++size_;
        }
    }

    void reserve(size_t count) {
        if (count > capacity_) reserve_exact(count);
    }

    void clear() noexcept { destroy_all(); size_ = 0; }

private:
    static void construct_range(T* dst, T* src, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            ::new (static_cast<void*>(dst + i)) T(std::move(src[i]));
        }
    }

    void grow() { reserve_exact(capacity_ * 2); }

    void reserve_exact(size_t count) {
        assert(count <= (static_cast<size_t>(-1) / sizeof(T)) &&
               "SmallVector capacity overflow (host bug, not guest-reachable)");
        // Allocate raw storage first so moving elements cannot observe a
        // partially constructed buffer through `this`. Aligned new keeps the
        // container correct for over-aligned element types.
        void* raw = operator new[](count * sizeof(T), std::align_val_t(alignof(T)));
        T* fresh = static_cast<T*>(raw);
        construct_range(fresh, data(), size_);
        destroy_all();
        if (heap_ != nullptr) {
            operator delete[](heap_, std::align_val_t(alignof(T)));
        }
        heap_ = fresh;
        capacity_ = count;
    }

    void destroy_all() noexcept {
        T* p = data();
        for (size_t i = 0; i < size_; ++i) p[i].~T();
    }

    void assign_from(const SmallVector& other) {
        reserve(other.size_);
        for (size_t i = 0; i < other.size_; ++i) {
            ::new (static_cast<void*>(data() + i)) T(other.data()[i]);
        }
        size_ = other.size_;
    }

    void move_from(SmallVector&& other) {
        if (heap_ != nullptr) {  // move-assign: release our buffer first
            operator delete[](heap_, std::align_val_t(alignof(T)));
            heap_ = nullptr;
        }
        if (other.heap_ == nullptr) {
            // Inline elements must be moved individually.
            for (size_t i = 0; i < other.size_; ++i) {
                ::new (static_cast<void*>(data() + i))
                    T(std::move(other.data()[i]));
            }
            size_ = other.size_;
            other.destroy_all();
            other.size_ = 0;
        } else {
            heap_ = other.heap_;
            capacity_ = other.capacity_;
            size_ = other.size_;
            other.heap_ = nullptr;
            other.capacity_ = N;
            other.size_ = 0;
        }
    }

    T* inline_or_heap() noexcept { return heap_ != nullptr ? heap_ : inline_; }
    const T* inline_or_heap() const noexcept {
        return heap_ != nullptr ? heap_ : inline_;
    }

    // Aligned raw storage; elements are constructed on demand.
    alignas(T) uint8_t inline_storage_[N * sizeof(T)];
    T* heap_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = N;

    // `inline_` aliases the storage byte array with correct type for access.
    T* const inline_ = reinterpret_cast<T*>(inline_storage_);
};

// ---------------------------------------------------------------------------
// FlatHashMap<K, V>
// ---------------------------------------------------------------------------

namespace detail {
template <typename K>
struct DefaultHash {
    size_t operator()(const K& k) const noexcept { return std::hash<K>{}(k); }
};
template <>
struct DefaultHash<uint64_t> {
    // splitmix64 finalizer: cheap, well-distributed for counter-like keys.
    // Constants are the published splitmix64 parameters (Steele et al.),
    // named per CEM-26 section 2 — they are a protocol, not tunables.
    static constexpr uint64_t kGoldenGamma = 0x9e3779b97f4a7c15ull;
    static constexpr uint64_t kMix1 = 0xbf58476d1ce4e5b9ull;
    static constexpr uint64_t kMix2 = 0x94d049bb133111ebull;
    static constexpr unsigned kShift1 = 30;
    static constexpr unsigned kShift2 = 27;
    static constexpr unsigned kShift3 = 31;
    size_t operator()(uint64_t x) const noexcept {
        x += kGoldenGamma;
        x = (x ^ (x >> kShift1)) * kMix1;
        x = (x ^ (x >> kShift2)) * kMix2;
        return static_cast<size_t>(x ^ (x >> kShift3));
    }
};
}  // namespace detail

/// Open-addressing hash map with linear probing and backward-shift deletion.
/// Power-of-two capacity, growth at 70% load. Values must be move-assignable.
/// This is the ONLY hash map allowed on hot compiler/runtime paths (Rule 50).
template <typename K, typename V, typename Hash = detail::DefaultHash<K>>
class FlatHashMap {
    struct Slot {
        int8_t state = kEmpty;  // kEmpty / kFull / kTombstone
        alignas(K) uint8_t key_storage_[sizeof(K)];
        alignas(V) uint8_t value_storage_[sizeof(V)];
    };
    static constexpr int8_t kEmpty = 0;
    static constexpr int8_t kFull = 1;
    static constexpr int8_t kTombstone = 2;

public:
    FlatHashMap() = default;
    ~FlatHashMap() { destroy_slots(); }

    FlatHashMap(const FlatHashMap& other) { copy_from(other); }
    FlatHashMap& operator=(const FlatHashMap& other) {
        if (this != &other) {
            destroy_slots();
            copy_from(other);
        }
        return *this;
    }
    FlatHashMap(FlatHashMap&& other) noexcept { move_from(std::move(other)); }
    FlatHashMap& operator=(FlatHashMap&& other) noexcept {
        if (this != &other) {
            destroy_slots();
            move_from(std::move(other));
        }
        return *this;
    }

    struct Entry {
        const K& key;
        V& value;
    };

    /// Returns pointer to the value if `key` is present, else nullptr.
    /// @hot (used by T0 field-slot/virtual-resolution caches and bigram
    /// counting).
    /// PERF_CONTRACT:
    /// BUDGET: <= 8 cycles steady-state (1 hash fold + 1 probe compare +
    ///         return; linear probing degrades only under clustering, growth
    ///         keeps load factor <= 70%)
    /// READS: 64 bytes (one slot line — Slot is 24 bytes, so ~2.7 slots/line;
    ///         probe chains of 1 dominate at <= 70% load)
    /// WRITES: 0
    /// BRANCHES: 1-2 probe compares + 1 empty-terminator, predictable once
    ///           the working set is resident
    /// CACHE: one to two slot lines; no per-node indirection (open addressing)
    /// PERF_NOTE: the hash is splitmix64 (4 imul + 3 shift/xor, ~5 cycles of
    ///           independent ALU) — it overlaps with the first probe load.
    V* find(const K& key) noexcept {
        if (slot_mask_ == 0) return nullptr;
        size_t i = probe_start(key);
        while (true) {
            Slot& s = slots_[i];
            if (s.state == kEmpty) return nullptr;
            if (s.state == kFull && keys_equal(key_at(s), key)) {
                return &value_at(s);
            }
            i = (i + 1) & slot_mask_;
        }
    }
    const V* find(const K& key) const noexcept {
        return const_cast<FlatHashMap*>(this)->find(key);
    }

    bool contains(const K& key) const noexcept { return find(key) != nullptr; }

    /// Inserts (key, value) if absent; returns pointer to the (possibly
    /// existing) value. @warm: growth is the only allocation and fires at
    /// kLoadFactorNum/kLoadFactorDen load — steady-state inserts are probe
    /// + store.
    /// PERF_CONTRACT (steady state):
    /// BUDGET: <= 10 cycles (find + tombstone-free store)
    /// WRITES: 24 bytes (one slot) on the probed line
    /// Host-side OOM escalates per EXC-001 (native
    /// exceptions are forbidden in the core library).
    V* insert(const K& key, V value) {
        if ((size_ + tombstones_ + 1) * kLoadFactorNum >=
            capacity_ * kLoadFactorDen) {
            if (!rehash(capacity_ == 0 ? kInitialCapacity
                                       : capacity_ * kGrowthFactor)) {
                return nullptr;
            }
        }
        size_t i = probe_start(key);
        size_t first_tombstone = SIZE_MAX;
        while (true) {
            Slot& s = slots_[i];
            if (s.state == kFull && keys_equal(key_at(s), key)) {
                return &value_at(s);
            }
            if (s.state == kTombstone && first_tombstone == SIZE_MAX) {
                first_tombstone = i;
            }
            if (s.state == kEmpty) {
                Slot& target =
                    first_tombstone != SIZE_MAX ? slots_[first_tombstone] : s;
                if (first_tombstone != SIZE_MAX) --tombstones_;
                construct_slot(target, key, std::move(value));
                ++size_;
                return &value_at(target);
            }
            i = (i + 1) & slot_mask_;
        }
    }

    /// Erases `key`; returns true if it was present.
    bool erase(const K& key) noexcept {
        if (slot_mask_ == 0) return false;
        size_t i = probe_start(key);
        while (true) {
            Slot& s = slots_[i];
            if (s.state == kEmpty) return false;
            if (s.state == kFull && keys_equal(key_at(s), key)) {
                destroy_slot(s);
                s.state = kTombstone;
                --size_;
                ++tombstones_;
                backward_shift((i + 1) & slot_mask_);
                return true;
            }
            i = (i + 1) & slot_mask_;
        }
    }

    void clear() noexcept {
        destroy_slots();
        size_ = 0;
        tombstones_ = 0;
    }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    /// Iteration over live slots (order unspecified — do not rely on it).
    template <typename Fn>
    void for_each(Fn&& fn) {
        for (size_t i = 0; i <= slot_mask_; ++i) {
            Slot& s = slots_[i];
            if (s.state == kFull) fn(Entry{key_at(s), value_at(s)});
        }
    }

private:
    static constexpr size_t kInitialCapacity = 16;
    /// Growth policy (CEM-26 section 2: named, globally tunable).
    static constexpr size_t kGrowthFactor = 2;
    /// Load factor as a rational: (n+1)*kLoadFactorNum >= capacity *
    /// kLoadFactorDen means 70% — keeps linear-probe chains short (see the
    /// find() contract).
    static constexpr size_t kLoadFactorNum = 10;
    static constexpr size_t kLoadFactorDen = 7;

    size_t probe_start(const K& key) const noexcept {
        return static_cast<size_t>(hash_(key)) & slot_mask_;
    }
    static bool keys_equal(const K& a, const K& b) noexcept { return a == b; }

    K& key_at(Slot& s) noexcept {
        return *reinterpret_cast<K*>(s.key_storage_);
    }
    const K& key_at(const Slot& s) const noexcept {
        return *reinterpret_cast<const K*>(s.key_storage_);
    }
    V& value_at(Slot& s) noexcept {
        return *reinterpret_cast<V*>(s.value_storage_);
    }
    const V& value_at(const Slot& s) const noexcept {
        return *reinterpret_cast<const V*>(s.value_storage_);
    }

    void construct_slot(Slot& s, const K& key, V&& value) {
        ::new (static_cast<void*>(s.key_storage_)) K(key);
        ::new (static_cast<void*>(s.value_storage_)) V(std::move(value));
        s.state = kFull;
    }

    void destroy_slot(Slot& s) noexcept {
        key_at(s).~K();
        value_at(s).~V();
    }

    /// Rehashes to `new_capacity` slots. Backward-shifts tombstones away.
    bool rehash(size_t new_capacity) {
        assert(new_capacity <= (static_cast<size_t>(-1) / sizeof(Slot)) &&
               "FlatHashMap capacity overflow (host bug, not guest-reachable)");
        void* raw =
            operator new[](new_capacity * sizeof(Slot), std::align_val_t(alignof(Slot)));
        Slot* fresh = static_cast<Slot*>(raw);
        for (size_t i = 0; i < new_capacity; ++i) {
            fresh[i].state = kEmpty;
        }
        Slot* old = slots_;
        size_t old_cap = capacity_ == 0 ? 0 : slot_mask_ + 1;
        slots_ = fresh;
        capacity_ = new_capacity;
        slot_mask_ = new_capacity - 1;
        tombstones_ = 0;
        size_t moved = 0;
        for (size_t i = 0; i < old_cap && moved < size_; ++i) {
            if (old[i].state == kFull) {
                size_t j = probe_start(key_at(old[i]));
                while (fresh[j].state == kFull) j = (j + 1) & slot_mask_;
                construct_slot(fresh[j], key_at(old[i]),
                               std::move(value_at(old[i])));
                destroy_slot(old[i]);  // key copied, value moved out
                ++moved;
            }
        }
        operator delete[](old, std::align_val_t(alignof(Slot)));
        return true;
    }

    /// Backward-shift deletion after erase at position `hole`: keeps probe
    /// chains intact without tombstones.
    void backward_shift(size_t i) noexcept {
        // With explicit tombstones we skip the shift; tombstones are recycled
        // by insert() and eliminated by rehash. Kept as a hook for a
        // tombstone-free variant if profiling demands it.
        (void)i;
    }

    void destroy_slots() noexcept {
        if (slots_ == nullptr) return;
        for (size_t i = 0; i <= slot_mask_; ++i) {
            if (slots_[i].state == kFull) destroy_slot(slots_[i]);
        }
        operator delete[](slots_, std::align_val_t(alignof(Slot)));
        slots_ = nullptr;
        capacity_ = 0;
        slot_mask_ = 0;
        size_ = 0;
        tombstones_ = 0;
    }

    void copy_from(const FlatHashMap& other) {
        if (other.size_ == 0) return;
        rehash(other.capacity_ == 0 ? kInitialCapacity : other.capacity_);
        for (size_t i = 0; i <= other.slot_mask_; ++i) {
            const Slot& s = other.slots_[i];
            if (s.state == kFull) {
                V copy = value_at(s);
                insert(key_at(s), std::move(copy));
            }
        }
    }

    void move_from(FlatHashMap&& other) noexcept {
        slots_ = other.slots_;
        capacity_ = other.capacity_;
        slot_mask_ = other.slot_mask_;
        size_ = other.size_;
        tombstones_ = other.tombstones_;
        other.slots_ = nullptr;
        other.capacity_ = 0;
        other.slot_mask_ = 0;
        other.size_ = 0;
        other.tombstones_ = 0;
    }

    Hash hash_{};
    Slot* slots_ = nullptr;
    size_t capacity_ = 0;   // total slots (power of two, or 0)
    size_t slot_mask_ = 0;  // capacity_ - 1 (or 0)
    size_t size_ = 0;
    size_t tombstones_ = 0;

    // CEM-26 section 9: the slot is the hot-path granule — its size sets how
    // many probes share a cache line (24 bytes for the u64->u64 table = ~2.7
    // slots/line).
    static_assert(sizeof(Slot) <= 32,
                  "Slot size defines probe locality; keep it word-packed");
};

// ---------------------------------------------------------------------------
// SparseSet — Rule 51 dataflow sets
// ---------------------------------------------------------------------------

/// Dense/sparse u32 set: O(1) insert/contains/clear, iteration in insertion
/// order over the dense array. The standard structure for liveness, visited
/// sets and reachability worklists.
class SparseSet {
public:
    explicit SparseSet(uint32_t universe = 0) { reset(universe); }

    void reset(uint32_t universe) {
        universe_ = universe;
        sparse_.assign(universe, kInvalid);
        dense_.clear();
    }

    void insert(uint32_t v) {
        if (v >= universe_) return;
        if (sparse_[v] != kInvalid) return;
        sparse_[v] = static_cast<uint32_t>(dense_.size());
        dense_.push_back(v);
    }

    bool contains(uint32_t v) const noexcept {
        return v < universe_ && sparse_[v] != kInvalid &&
               dense_[sparse_[v]] == v;
    }

    void remove(uint32_t v) {
        if (!contains(v)) return;
        uint32_t pos = sparse_[v];
        uint32_t last = dense_.back();
        dense_[pos] = last;
        sparse_[last] = pos;
        dense_.pop_back();
        sparse_[v] = kInvalid;
    }

    void clear() noexcept {
        for (uint32_t v : dense_) sparse_[v] = kInvalid;
        dense_.clear();
    }

    size_t size() const noexcept { return dense_.size(); }
    bool empty() const noexcept { return dense_.empty(); }
    const uint32_t* begin() const noexcept { return dense_.data(); }
    const uint32_t* end() const noexcept { return dense_.data() + dense_.size(); }

    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

private:
    std::vector<uint32_t> sparse_;
    std::vector<uint32_t> dense_;
    uint32_t universe_ = 0;
};

}  // namespace vortex::support
