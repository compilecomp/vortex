#include "vortex/infra/code_cache.hpp"

#include <algorithm>

namespace vortex::infra {

CodeCache::CodeCache(size_t total_bytes) {
    auto mem = WritableCodeMemory::allocate(total_bytes);
    if (mem.has_value()) {
        memory_ = std::move(*mem);
    }
}

support::Result<CodeAllocation> CodeCache::allocate(
    CodeSegmentKind kind, vortex::Tier tier, uint32_t method_id,
    std::span<const uint8_t> code) {
    if (!memory_.data() || used_ + code.size() > memory_.size()) {
        return support::fail(support::ErrorCode::OutOfMemory,
                             "code cache exhausted");
    }
    const size_t offset = used_;
    memory_.write(code, offset);
    used_ += (code.size() + 15) & ~size_t{15};  // 16-byte alignment per allocation

    CodeAllocation alloc;
    alloc.id = static_cast<uint32_t>(allocations_.size());
    alloc.kind = kind;
    alloc.tier = tier;
    alloc.method_id = method_id;
    alloc.offset = offset;
    alloc.size = code.size();
    allocations_.push_back(alloc);
    return alloc;
}

const uint8_t* CodeCache::code_of(const CodeAllocation& alloc) const noexcept {
    if (!memory_.data() || alloc.offset + alloc.size > memory_.size()) {
        return nullptr;
    }
    return memory_.data() + alloc.offset;
}

support::Result<size_t> CodeSweeper::sweep_cold_regions() {
    return support::unimplemented("code sweeper / compaction");
}

}  // namespace vortex::infra
