#include "vortex/infra/ffi.hpp"

namespace vortex::infra {

support::Result<uint32_t> HandleTable::create(void* object, HandleScope scope) {
    uint32_t id;
    if (!free_list_.empty()) {
        id = free_list_.back();
        free_list_.pop_back();
    } else {
        id = static_cast<uint32_t>(entries_.size());
        entries_.emplace_back();
    }
    entries_[id] = Entry{object, scope, true};
    ++live_;
    return id;
}

support::Result<void*> HandleTable::resolve(uint32_t handle) const {
    if (handle >= entries_.size() || !entries_[handle].alive) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "resolve: dead or out-of-range handle " +
                                 std::to_string(handle));
    }
    return entries_[handle].object;
}

void HandleTable::destroy(uint32_t handle) {
    if (handle < entries_.size() && entries_[handle].alive) {
        entries_[handle].alive = false;
        entries_[handle].object = nullptr;
        free_list_.push_back(handle);
        --live_;
    }
}

size_t HandleTable::destroy_scope(HandleScope scope) {
    size_t freed = 0;
    for (uint32_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].alive && entries_[i].scope == scope) {
            destroy(i);
            ++freed;
        }
    }
    return freed;
}

void PinningRegion::unpin(void* object) {
    for (auto it = pinned_.begin(); it != pinned_.end(); ++it) {
        if (*it == object) {
            pinned_.erase(it);
            return;
        }
    }
}

bool PinningRegion::is_pinned(const void* object) const noexcept {
    for (const void* p : pinned_) {
        if (p == object) return true;
    }
    return false;
}

}  // namespace vortex::infra
