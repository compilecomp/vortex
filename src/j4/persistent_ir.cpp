// CEP:FILE: src/j4/persistent_ir.cpp
// CEP:WHAT: Persistent IR store implementation: identity walks and the keyed reuse protocol.
// CEP:WHY: The spec 12.1 reuse rule lives here - exact hash comparison, drop-on-stale, keyed-only access.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: All paths are exception-free; stale entries are erased in place.
// CEP:ASSUMES: support/hash.hpp's FNV protocol constants; single-mutator access.
// CEP:COST: @cold. Per compile: two O(bytecode) hash walks (module identity + method version) + one bounded linear store match.
// CEP:EVIDENCE: tests/test_j4.cpp - the persistent-IR test pair.
#include "vortex/j4/persistent_ir.hpp"

#include "vortex/support/hash.hpp"

namespace vortex::j4 {

/// CEP:WHAT: FNV-1 over the module's method table (ids, lengths, code bytes).
/// CEP:WHY: Content-addressed module identity (Rule 5): stable across reloads of the same artifact, blind to heap addresses.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure function).
/// CEP:ASSUMES: method-table order is stable for a given module artifact.
/// CEP:COST: @cold. O(total bytecode) with one multiply-xor per byte; at most a few KB per module.
/// CEP:EVIDENCE: j4_persistent_ir_reuse_is_bit_identical (identical modules share store keys).
uint64_t PersistentIrStore::module_identity(
    const ugb::UGBModule& module) noexcept {
    uint64_t h = support::kFnv1aBasis;
    h = support::fnv1a_mix(h, module.method_table.size());
    for (const ugb::UGBMethod& m : module.method_table) {
        h = support::fnv1a_mix(h, m.id);
        h = support::fnv1a_mix(h, m.code.size());
        for (const uint8_t b : m.code) {
            h = support::fnv1a_mix(h, b);
        }
    }
    return h;
}

/// CEP:WHAT: FNV-1 over one method's header words and code bytes.
/// CEP:WHY: The version identity the reuse rule compares - any bytecode change flips it (spec 12.1: never guess staleness).
/// CEP:STATUS: complete
/// CEP:FAILURE: Returns 0 for an out-of-range method id (reuse fails closed).
/// CEP:ASSUMES: register_count and code size participate so even same-length edits flip the hash.
/// CEP:COST: @cold. O(code bytes) per call, twice per compile.
/// CEP:EVIDENCE: j4_persistent_ir_invalidation_is_content_addressed (one padding byte flips the hash).
uint64_t PersistentIrStore::method_version_hash(
    const ugb::UGBModule& module, uint32_t method_id) noexcept {
    if (method_id >= module.method_table.size()) return 0;
    const ugb::UGBMethod& m = module.method_table[method_id];
    uint64_t h = support::kFnv1aBasis;
    h = support::fnv1a_mix(h, m.id);
    h = support::fnv1a_mix(h, m.register_count);
    h = support::fnv1a_mix(h, m.code.size());
    for (const uint8_t b : m.code) {
        h = support::fnv1a_mix(h, b);
    }
    return h;
}

/// CEP:WHAT: The store key: module identity mixed with the method id.
/// CEP:WHY: One 64-bit keyed identity (Rule 5) keeps the store enumeration-free (Rule 16).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure function).
/// CEP:ASSUMES: none beyond the hash protocol.
/// CEP:COST: @cold. Two multiply-xor mixes.
/// CEP:EVIDENCE: exercised by every store-path test.
uint64_t PersistentIrStore::store_key(const ugb::UGBModule& module,
                                      uint32_t method_id) noexcept {
    return support::fnv1a_mix(module_identity(module),
                              static_cast<uint64_t>(method_id));
}

/// CEP:WHAT: Keyed reuse lookup with drop-on-stale and drop-on-half-dead.
/// CEP:WHY: Reuse must be exact (present + hash-identical + not invalidated + a live graph); anything else must be REMOVED so the caller rebuilds from bytecode (spec 12.1: replace, never patch).
/// CEP:STATUS: complete
/// CEP:FAILURE: Returns nullptr when absent/stale/invalidated/half-dead - the caller rebuilds; no error surface.
/// CEP:ASSUMES: the caller moves the graph out on success and returns it via put().
/// CEP:COST: @cold. One bounded linear match (<= dozens of entries); the erase paths move one element (O(n)).
/// CEP:EVIDENCE: j4_persistent_ir_invalidation_is_content_addressed (all three null paths); M4 review 13b (the half-dead path keeps reused_ir telemetry truthful, Rule 120).
PersistentIrEntry* PersistentIrStore::find_reusable(
    uint64_t key, uint64_t bytecode_version_hash) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        PersistentIrEntry& entry = entries_[i];
        if (entry.key != key) continue;
        if (entry.state == PersistentIrEntry::State::Invalidated ||
            entry.bytecode_version_hash != bytecode_version_hash) {
            // Stale: replace, never patch (spec 12.1). The caller rebuilds.
            entries_.erase(entries_.begin() +
                           static_cast<std::ptrdiff_t>(i));
            return nullptr;
        }
        if (entry.built == nullptr) {
            // Half-dead: a previous capture moved the graph out (failed
            // compile, or a worker cancelled between capture and put).
            // Serving it would report a reuse that rebuilds anyway — a
            // Rule-120 telemetry lie — so erase and rebuild.
            entries_.erase(entries_.begin() +
                           static_cast<std::ptrdiff_t>(i));
            return nullptr;
        }
        return &entry;
    }
    return nullptr;
}

/// CEP:WHAT: Insert-or-replace of a store entry.
/// CEP:WHY: The write side of the reuse protocol - compiles and the worker return graph states here.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (move-assign or push_back; OOM escalates per EXC-001).
/// CEP:ASSUMES: the key parameter names the entry (copied into it).
/// CEP:COST: @cold. One linear match + one move-assign or amortized push_back.
/// CEP:EVIDENCE: j4_persistent_ir_reuse_is_bit_identical.
void PersistentIrStore::put(uint64_t key, PersistentIrEntry entry) {
    for (PersistentIrEntry& slot : entries_) {
        if (slot.key == key) {
            slot = std::move(entry);
            return;
        }
    }
    entry.key = key;
    entries_.push_back(std::move(entry));
}

/// CEP:WHAT: Records external invalidation of one entry.
/// CEP:WHY: Rule 15 - compilation must be cancellable on invalidation; the dependency engine marks entries retired without freeing mid-flight state.
/// CEP:STATUS: complete
/// CEP:FAILURE: Missing keys are a no-op (the caller owns the callback contract).
/// CEP:ASSUMES: the next find_reusable drops Invalidated entries.
/// CEP:COST: @cold. One linear match + one byte store.
/// CEP:EVIDENCE: j4_persistent_ir_invalidation_is_content_addressed.
void PersistentIrStore::invalidate(uint64_t key) {
    for (PersistentIrEntry& entry : entries_) {
        if (entry.key == key) {
            entry.state = PersistentIrEntry::State::Invalidated;
            return;
        }
    }
}

}  // namespace vortex::j4
