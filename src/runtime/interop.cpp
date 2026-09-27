// The Interop Message Protocol — registry + tagged dispatch
// (docs/interop-protocol.md sections 4-7; docs/roadmap.md M3).
//
// The hot path here is one FlatHashMap probe (klass word -> language id)
// plus one indirect call through the language's vtable slot — the same
// shape the builtin invoke ABI uses (PERF-002 pattern: the call IS the
// operation). Everything else (string names, registration) is @cold.
#include "vortex/runtime/interop.hpp"

#include "vortex/runtime/object_model.hpp"

namespace vortex::runtime::interop {

uint16_t InteropRegistry::register_language(std::string_view name,
                                            const InteropVTable& vtable,
                                            uint32_t capabilities,
                                            PortKind kind) {
    // Language ids are 1-based; 0 stays the invalid id so a default-
    // constructed uint16_t never accidentally names a port.
    Language& lang = languages_.emplace_back();
    lang.name = std::string(name);
    lang.vtable = vtable;
    lang.capabilities = capabilities;
    lang.kind = kind;
    return static_cast<uint16_t>(languages_.size());
}

support::Result<void> InteropRegistry::bind_klass(uint16_t language_id,
                                                  const void* klass) {
    if (language_id == 0 || language_id > languages_.size()) {
        return support::fail(
            support::ErrorCode::InvalidArgument,
            "bind_klass: unknown language id " + std::to_string(language_id));
    }
    if (klass == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "bind_klass: null klass");
    }
    // Frozen binding (Rules 30/31): the same (klass, language) pair is
    // idempotent; a DIFFERENT language for a bound klass is a named error.
    // Installed JIT code speculates on the port kind of a bound klass with
    // no runtime re-check — the freeze IS the invalidation dependency
    // (a silent last-wins rebind would strand that speculation).
    if (const uint16_t* existing = klass_to_language_.find(klass)) {
        if (*existing == language_id) return support::ok();
        return support::fail(
            support::ErrorCode::InvalidState,
            "bind_klass: klass already bound to language '" +
                std::string(language_name(*existing)) + "' (id " +
                std::to_string(*existing) +
                "); port-kind bindings are frozen once made (Rules 30/31)");
    }
    klass_to_language_.insert(klass, language_id);
    return support::ok();
}

uint16_t InteropRegistry::language_id(std::string_view name) const noexcept {
    for (size_t i = 0; i < languages_.size(); ++i) {
        if (languages_[i].name == name) {
            return static_cast<uint16_t>(i + 1);
        }
    }
    return 0;
}

const char* InteropRegistry::language_name(uint16_t id) const noexcept {
    return id != 0 && id <= languages_.size()
               ? languages_[id - 1].name.c_str()
               : nullptr;
}

uint32_t InteropRegistry::capabilities_of(uint16_t id) const noexcept {
    return id != 0 && id <= languages_.size() ? languages_[id - 1].capabilities
                                              : 0;
}

const InteropVTable* InteropRegistry::vtable_of(uint16_t id) const noexcept {
    return id != 0 && id <= languages_.size() ? &languages_[id - 1].vtable
                                              : nullptr;
}

uint16_t InteropRegistry::language_of_klass(const void* klass) const noexcept {
    const uint16_t* id = klass_to_language_.find(klass);
    return id != nullptr ? *id : 0;
}

PortKind InteropRegistry::port_kind_of_klass(const void* klass) const noexcept {
    const uint16_t* id = klass_to_language_.find(klass);
    if (id == nullptr) return PortKind::Foreign;
    return languages_[*id - 1].kind;
}

// ---- receiver resolution ------------------------------------------------------

uint16_t receiver_language(const InteropRegistry& registry,
                           const TaggedValue& recv) {
    if (!recv.is_heap_object()) return 0;
    const auto* obj = recv.as_heap_object();
    if (obj->header.klass == nullptr) return 0;
    return registry.language_of_klass(obj->header.klass);
}

namespace {

support::Result<std::pair<uint16_t, const InteropVTable*>> resolve(
    const InteropRegistry& registry, const TaggedValue& recv,
    const char* what) {
    if (!recv.is_heap_object()) {
        return support::fail(support::ErrorCode::TypeError,
                             std::string(what) + ": receiver is not an object");
    }
    const auto* obj = recv.as_heap_object();
    if (obj->header.klass == nullptr) {
        return support::fail(support::ErrorCode::TypeError,
                             std::string(what) +
                                 ": receiver has no klass");
    }
    const uint16_t lang = registry.language_of_klass(obj->header.klass);
    if (lang == 0) {
        return support::fail(support::ErrorCode::Unsupported,
                             std::string(what) +
                                 ": receiver klass is not a registered "
                                 "interop language");
    }
    return std::make_pair(lang, registry.vtable_of(lang));
}

// fail() returns a type-erased unexpected convertible into any Result<T>.
std::unexpected<support::Diagnostic> unsupported() {
    return support::fail(support::ErrorCode::Unsupported,
                         std::string(kInteropUnsupportedMessage));
}

}  // namespace

namespace {
// Rule 9/30: a vortex-heap receiver's member_idx IS a field slot — the
// port reads Object::field(i) unchecked, so the dispatch validates the slot
// against the klass layout before handing the object over. Foreign-memory
// ports (non-vortex receivers) keep the raw path by contract.
support::Result<void> validate_member_idx(const TaggedValue& recv,
                                          uint32_t member_idx,
                                          const char* what) {
    const auto* obj = recv.as_heap_object();
    const auto* k = obj->header.klass;
    if (k != nullptr && member_idx >= k->field_count()) {
        return support::fail(
            support::ErrorCode::TypeError,
            std::string(what) + ": member_idx " +
                std::to_string(member_idx) + " out of range for klass '" +
                k->name() + "' (field_count " +
                std::to_string(k->field_count()) + ")");
    }
    return support::ok();
}
}  // namespace

support::Result<TaggedValue> dispatch_read_member(const InteropRegistry& registry,
                                                  const TaggedValue& recv,
                                                  uint32_t member_idx) {
    auto lang = resolve(registry, recv, "Poly.Read");
    if (!lang) return std::unexpected(lang.error());
    // Capability gate FIRST (docs sections 3-4): a language without the
    // members group does not service member messages at all, so the
    // refusal is named (UNSUPPORTED) regardless of slot or receiver state
    // — even with a non-null slot from a misconfigured port and even for
    // an out-of-range member_idx (the slot geometry of an unserviced
    // message is not a TypeError surface).
    if ((registry.capabilities_of(lang->first) & CAP_INTEROP_MEMBERS) == 0) {
        return unsupported();
    }
    if (auto v = validate_member_idx(recv, member_idx, "Poly.Read"); !v) {
        return std::unexpected(v.error());
    }
    if (lang->second->read_member == nullptr) return unsupported();
    void* raw = static_cast<void*>(recv.as_heap_object());
    const uint64_t bits = lang->second->read_member(raw, member_idx);
    // read_member returns the tagged WORD of the member. A port with no
    // value to return uses the canonical undefined word (TaggedValue::
    // undefined) — the same sentinel contract as the generic-binop helper.
    return TaggedValue::from_raw(bits);
}

support::Result<void> dispatch_write_member(const InteropRegistry& registry,
                                            const TaggedValue& recv,
                                            uint32_t member_idx,
                                            const TaggedValue& value) {
    auto lang = resolve(registry, recv, "Poly.Write");
    if (!lang) return std::unexpected(lang.error());
    // Capability gate FIRST — same ordering contract as Poly.Read above.
    if ((registry.capabilities_of(lang->first) & CAP_INTEROP_MEMBERS) == 0) {
        return unsupported();
    }
    if (auto v = validate_member_idx(recv, member_idx, "Poly.Write"); !v) {
        return std::unexpected(v.error());
    }
    if (lang->second->write_member == nullptr) return unsupported();
    void* raw = static_cast<void*>(recv.as_heap_object());
    // Barrier note (docs section 4): interop wrappers are written through
    // the port's own logic; when the wrapper is a vortex heap object the
    // port performs the card mark (the tagged surface cannot know the
    // wrapper's generation).
    lang->second->write_member(raw, member_idx, value.raw());
    return support::ok();
}

support::Result<TaggedValue> dispatch_execute(const InteropRegistry& registry,
                                              const TaggedValue& recv,
                                              std::span<const TaggedValue> args) {
    auto lang = resolve(registry, recv, "Poly.Execute");
    if (!lang) return std::unexpected(lang.error());
    if ((registry.capabilities_of(lang->first) & CAP_INTEROP_EXECUTE) == 0) {
        return unsupported();
    }
    if (lang->second->execute == nullptr) return unsupported();
    TaggedValue ret = TaggedValue::undefined();
    void* raw = static_cast<void*>(recv.as_heap_object());
    const int64_t rc = lang->second->execute(
        raw, args.empty() ? nullptr : args.data(),
        static_cast<uint32_t>(args.size()), &ret);
    if (rc != 0) {
        return support::fail(support::ErrorCode::RuntimeError,
                             "Poly.Execute: language handler failed with " +
                                 std::to_string(rc));
    }
    return ret;
}

support::Result<TaggedValue> dispatch_send(const InteropRegistry& registry,
                                           uint16_t message_id,
                                           const TaggedValue& recv,
                                           std::span<const TaggedValue> args) {
    // The generic send routes by message id (docs section 2). Only the
    // canonical messages with a 1:1 vtable slot are routed here; anything
    // else is UNSUPPORTED — never silently dropped (Rule 3).
    const auto arg_at = [&](size_t i) -> TaggedValue {
        return i < args.size() ? args[i] : TaggedValue::undefined();
    };
    switch (message_id) {
    case MSG_HAS_MEMBERS: {
        auto lang = resolve(registry, recv, "Poly.Send(HAS_MEMBERS)");
        if (!lang) return std::unexpected(lang.error());
        // Capability gate FIRST (same ordering contract as Poly.Read —
        // HAS_MEMBERS is a members-group message).
        if ((registry.capabilities_of(lang->first) & CAP_INTEROP_MEMBERS) ==
            0) {
            return unsupported();
        }
        if (lang->second->has_members == nullptr) return unsupported();
        void* raw = static_cast<void*>(recv.as_heap_object());
        const TaggedValue idx = arg_at(0);
        if (!idx.is_smi() || idx.as_smi() < 0) {
            return support::fail(support::ErrorCode::TypeError,
                                 "Poly.Send(HAS_MEMBERS): member_idx not a smi");
        }
        const uint32_t has = lang->second->has_members(
            raw, static_cast<uint32_t>(idx.as_smi()));
        return TaggedValue::smi(has ? 1 : 0);
    }
    case MSG_READ_MEMBER: {
        const TaggedValue idx = arg_at(0);
        if (!idx.is_smi() || idx.as_smi() < 0) {
            return support::fail(support::ErrorCode::TypeError,
                                 "Poly.Send(READ_MEMBER): member_idx not a smi");
        }
        return dispatch_read_member(registry, recv,
                                    static_cast<uint32_t>(idx.as_smi()));
    }
    case MSG_WRITE_MEMBER: {
        const TaggedValue idx = arg_at(0);
        if (!idx.is_smi() || idx.as_smi() < 0) {
            return support::fail(support::ErrorCode::TypeError,
                                 "Poly.Send(WRITE_MEMBER): member_idx not a smi");
        }
        if (auto w = dispatch_write_member(
                registry, recv, static_cast<uint32_t>(idx.as_smi()),
                arg_at(1));
            !w) {
            return std::unexpected(w.error());
        }
        return TaggedValue::undefined();
    }
    case MSG_EXECUTE:
        // The locked contract: srcs = (recv, arg_base, argc) — the window
        // IS the pure argument list (recv rides in srcs[0], the window
        // never contains it). Same layout as POLY_EXECUTE.
        return dispatch_execute(registry, recv, args);
    case MSG_IS_NULL:
        return TaggedValue::smi(recv.is_null() ? 1 : 0);
    default:
        return unsupported();
    }
}

}  // namespace vortex::runtime::interop
