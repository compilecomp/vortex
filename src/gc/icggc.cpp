#include "vortex/gc/icggc.hpp"

#include <algorithm>
#include <cstring>

#include "vortex/support/log.hpp"

namespace vortex::gc {

namespace {
constexpr size_t kTlabChunk = 256 * 1024;
constexpr size_t kAlignment = 16;
}  // namespace

Heap::Heap(size_t young_bytes)
    : young_bytes_(young_bytes), tlab_(nullptr, 0), card_table_() {
    young_ = std::make_unique<uint8_t[]>(young_bytes_);
    std::memset(young_.get(), 0, young_bytes_);
    tlab_.reset(young_.get(), std::min(young_bytes_, kTlabChunk));
    card_table_ = CardTable(young_.get(), young_bytes_);

    // The boxed-double klass is a heap-owned singleton descriptor (it lives on
    // the C++ heap, not in the traced heap space).
    static Klass the_double_klass("double");
    double_klass_ = &the_double_klass;
}

Heap::~Heap() = default;

Result<void> Heap::refill_tlab(size_t min_bytes) {
    const uint8_t* heap_end = young_.get() + young_bytes_;
    uint8_t* next = tlab_.end();
    // Align up to 16.
    next = reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(next) + kAlignment - 1) & ~(kAlignment - 1));
    if (next + min_bytes > heap_end) {
        stats_.slow_path_allocations++;
        return support::fail(support::ErrorCode::OutOfMemory,
                             "young generation exhausted (M0: no young collection yet)");
    }
    tlab_.reset(next, std::min<size_t>(static_cast<size_t>(heap_end - next), kTlabChunk));
    stats_.tlab_refills++;
    return support::ok();
}

Result<Object*> Heap::allocate_object(Klass* klass, uint32_t field_count) {
    const size_t bytes =
        (sizeof(ObjectHeader) + sizeof(TaggedValue) * field_count + kAlignment - 1) &
        ~(kAlignment - 1);
    Object* obj = tlab_.try_allocate<Object>(bytes);
    if (obj == nullptr) {
        auto refill = refill_tlab(bytes);
        if (!refill) return std::unexpected(refill.error());
        obj = tlab_.try_allocate<Object>(bytes);
        if (obj == nullptr) {
            return support::fail(support::ErrorCode::OutOfMemory,
                                 "TLAB refill too small for object");
        }
    }
    obj->header.klass = klass;
    obj->header.size = static_cast<uint32_t>(bytes);
    // Zero fields so GC maps and interpreter reads observe well-defined nulls.
    for (uint32_t i = 0; i < field_count; ++i) obj->field(i) = TaggedValue::null();
    stats_.objects_allocated++;
    stats_.bytes_allocated += bytes;
    return obj;
}

Result<Object*> Heap::allocate_double(double value) {
    // HeapDouble layout: header + double payload.
    struct HeapDouble : Object {
        double value;
    };
    const size_t bytes =
        (sizeof(ObjectHeader) + sizeof(double) + kAlignment - 1) & ~(kAlignment - 1);
    HeapDouble* obj = tlab_.try_allocate<HeapDouble>(bytes);
    if (obj == nullptr) {
        auto refill = refill_tlab(bytes);
        if (!refill) return std::unexpected(refill.error());
        obj = tlab_.try_allocate<HeapDouble>(bytes);
        if (obj == nullptr) {
            return support::fail(support::ErrorCode::OutOfMemory,
                                 "TLAB refill too small for double box");
        }
    }
    obj->header.klass = double_klass_;
    obj->header.size = static_cast<uint32_t>(bytes);
    obj->value = value;
    stats_.objects_allocated++;
    stats_.bytes_allocated += bytes;
    return obj;
}

Result<ArrayObject*> Heap::allocate_array(uint32_t length) {
    const size_t bytes =
        (ArrayObject::bytes_for(length) + kAlignment - 1) & ~(kAlignment - 1);
    ArrayObject* obj = tlab_.try_allocate<ArrayObject>(bytes);
    if (obj == nullptr) {
        auto refill = refill_tlab(bytes);
        if (!refill) return std::unexpected(refill.error());
        obj = tlab_.try_allocate<ArrayObject>(bytes);
        if (obj == nullptr) {
            return support::fail(support::ErrorCode::OutOfMemory,
                                 "TLAB refill too small for array");
        }
    }
    obj->header.klass = nullptr;  // array type marker (M0)
    obj->header.size = static_cast<uint32_t>(bytes);
    obj->set_length(length);
    for (uint32_t i = 0; i < length; ++i) obj->element(i) = TaggedValue::null();
    stats_.objects_allocated++;
    stats_.bytes_allocated += bytes;
    return obj;
}

Result<void> Heap::collect_young() {
    return support::unimplemented("ICGGC young collection");
}

Result<void> Heap::collect_old_concurrent() {
    return support::unimplemented("ICGGC concurrent old-generation collection");
}

}  // namespace vortex::gc
