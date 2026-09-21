// Guest object model: klasses, shapes, objects, arrays (docs/ugb.md section 4.4,
// docs/gc-icggc.md section 2). M0 implements the fixed-klass model the reference
// frontend and the T0 interpreter execute against; shape transitions are part of
// the dynamic-object pack (docs/roadmap.md, M6).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/support/tagged_value.hpp"

namespace vortex {

class Klass;

/// Header word shared by every heap object. Kept GC-tracing-friendly: the klass
/// word is a raw pointer in M0 and becomes a compressed, tagged word when the
/// ICGGC concurrent engine lands (docs/roadmap.md, M5).
struct ObjectHeader {
    Klass* klass = nullptr;
    uint32_t size = 0;
    uint32_t mark : 1 = 0;      // used by future concurrent marking
    uint32_t forwarded : 1 = 0; // Brooks forwarding flag (docs/gc-icggc.md 4)
    uint32_t reserved : 30 = 0;
};

class HeapObject {
public:
    ObjectHeader header;

protected:
    ~HeapObject() = default;
};

/// A field slot descriptor inside a klass layout.
struct FieldSlot {
    std::string name;
    uint32_t offset = 0;  // byte offset from object base
};

/// Method entry kinds, mirroring UGB MethodToken.implementation_kind
/// (docs/ugb.md section 4.4).
enum class MethodImplementationKind : uint8_t {
    Bytecode = 0,
    Native,
    Intrinsic,
    RuntimeHook,
    MissingHook,
};

/// A minimal method table entry used by CallDirect lowering in the interpreter.
struct MethodEntry {
    std::string name;
    MethodImplementationKind kind = MethodImplementationKind::Bytecode;
    uint32_t ugb_method_id = 0;  // index into the UGB module's method table
    void* native_target = nullptr;
};

/// Klass — the static class descriptor. Objects point at it from their header.
class Klass {
public:
    Klass() = default;
    explicit Klass(std::string_view n) : name_(n) {}

    const std::string& name() const noexcept { return name_; }
    uint32_t id() const noexcept { return id_; }
    void set_id(uint32_t id) noexcept { id_ = id; }

    uint32_t field_count() const noexcept {
        return static_cast<uint32_t>(fields_.size());
    }
    const std::vector<FieldSlot>& fields() const noexcept { return fields_; }

    /// Returns the field index for `name`, or -1.
    int find_field(std::string_view name) const {
        for (uint32_t i = 0; i < fields_.size(); ++i) {
            if (fields_[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    /// Appends a field, assigning a contiguous slot offset.
    uint32_t add_field(std::string_view name) {
        FieldSlot slot;
        slot.name = std::string(name);
        slot.offset = sizeof(ObjectHeader) +
                      static_cast<uint32_t>(fields_.size()) * sizeof(TaggedValue);
        fields_.push_back(std::move(slot));
        return static_cast<uint32_t>(fields_.size() - 1);
    }

    std::vector<MethodEntry>& methods() noexcept { return methods_; }
    const std::vector<MethodEntry>& methods() const noexcept { return methods_; }

    MethodEntry* find_method(std::string_view name) {
        for (auto& m : methods_) {
            if (m.name == name) return &m;
        }
        return nullptr;
    }

private:
    std::string name_;
    uint32_t id_ = 0;
    std::vector<FieldSlot> fields_;
    std::vector<MethodEntry> methods_;
};

/// A regular object with a klass and a field array.
class Object : public HeapObject {
public:
    TaggedValue* fields_base() noexcept {
        return reinterpret_cast<TaggedValue*>(reinterpret_cast<uint8_t*>(this) +
                                             sizeof(ObjectHeader));
    }
    TaggedValue& field(uint32_t index) noexcept { return fields_base()[index]; }
};

/// Fixed-length tagged array (UGB Array.New / Array.Get / Array.Set).
/// Layout: header, 4-byte length, 4-byte pad, then TaggedValue elements —
/// the pad keeps every element 8-byte aligned (required by the tagged-value
/// scheme and any future atomic field access).
class ArrayObject : public HeapObject {
public:
    static constexpr uint32_t kLengthPadBytes = 8;  // length u32 + alignment pad

    static size_t bytes_for(uint32_t length) noexcept {
        return sizeof(ObjectHeader) + kLengthPadBytes +
               sizeof(TaggedValue) * length;
    }

    uint32_t length() const noexcept { return length_; }
    void set_length(uint32_t n) noexcept { length_ = n; }

    TaggedValue* elements_base() noexcept {
        return reinterpret_cast<TaggedValue*>(reinterpret_cast<uint8_t*>(this) +
                                             sizeof(ObjectHeader) +
                                             kLengthPadBytes);
    }
    TaggedValue& element(uint32_t i) noexcept { return elements_base()[i]; }

private:
    uint32_t length_ = 0;
    uint32_t pad_ = 0;
};

/// Registry of klasses for a loaded module set.
class KlassRegistry {
public:
    Klass* create(std::string_view name) {
        auto k = std::make_unique<Klass>(name);
        k->set_id(static_cast<uint32_t>(klasses_.size()));
        klasses_.push_back(std::move(k));
        return klasses_.back().get();
    }
    size_t size() const noexcept { return klasses_.size(); }
    Klass* at(size_t i) noexcept { return klasses_[i].get(); }

private:
    std::vector<std::unique_ptr<Klass>> klasses_;
};

/// Field/array slot store. Card marking for reference values is performed by
/// the caller against the heap's card table (docs/tier-t0.md section 8):
///   card_table.mark_dirty(owner)
inline void store_field(TaggedValue* slot, TaggedValue value) noexcept {
    *slot = value;
}

/// Boxed doubles live on the heap under the engine's `double` klass
/// (gc::Heap::double_klass). Layout: ObjectHeader + 8-byte payload.
inline bool is_boxed_double(const HeapObject* o, const Klass* double_klass) noexcept {
    return o != nullptr && double_klass != nullptr &&
           o->header.klass == double_klass;
}

inline double read_boxed_double(const HeapObject* o) noexcept {
    double v = 0.0;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(o) + sizeof(ObjectHeader), 8);
    return v;
}

inline void write_boxed_double(HeapObject* o, double value) noexcept {
    std::memcpy(reinterpret_cast<uint8_t*>(o) + sizeof(ObjectHeader), &value, 8);
}

}  // namespace vortex
