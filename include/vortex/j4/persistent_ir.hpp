// CEP:FILE: include/vortex/j4/persistent_ir.hpp
// CEP:WHAT: The persistent IR store: keyed reuse of pipeline-processed graph state across compiles (spec 12.1).
// CEP:WHY: Recompiles must start from the previous result instead of from bytecode - incremental reoptimization is a J4 tier property (docs/tier-j4.md section 5 item 24).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: All operations are exception-free on the lookup path; stale or invalidated entries are dropped (replaced, never patched).
// CEP:ASSUMES: Single-mutator access (tier-j4.md 12.3 stepwise-driver model); identity is content-addressed (bytecode hashes), so store behavior never depends on heap addresses.
// CEP:COST: @cold. Store size = one entry per J4-compiled method (dozens); lookup is a linear match over that bounded vector - deliberate and stated (the word-packed FlatHashMap is the WRONG container for whole-graph entries).
// CEP:EVIDENCE: j4_persistent_ir_reuse_is_bit_identical, j4_persistent_ir_invalidation_is_content_addressed (tests/test_j4.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "vortex/ir/escape_summary.hpp"
#include "vortex/j2/graph_builder.hpp"
#include "vortex/j4/max_jit.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::j4 {

/// CEP:WHAT: One store entry: the pipeline-processed graph state of a method plus identity and publication bookkeeping.
/// CEP:WHY: The reuse protocol needs the graph, its identity hash and its lifecycle in one movable unit (Rule 69: explicit ownership).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure struct; unique_ptr enforces single ownership of the graph state).
/// CEP:ASSUMES: the built graph is moved, never copied (an Arena cannot be copied).
/// CEP:COST: sizeof = one unique_ptr + hashes + summary; the graph payload lives on the heap once per compiled method.
/// CEP:EVIDENCE: exercised by every store-path test in tests/test_j4.cpp.
struct PersistentIrEntry {
    enum class State : uint8_t {
        Draft,        // captured/optimizing; never executed from
        Published,    // this state produced the currently published code
        Invalidated,  // dependency engine (or caller) retired this state
    };

    uint64_t key = 0;  // store key (module identity x method id)
    uint64_t bytecode_version_hash = 0;
    std::unique_ptr<j2::BuiltGraph> built;  // graph + per-node side tables
    ir::EscapeSummary summary;              // stage-29 output for this state
    FixedPointState fingerprint;            // last fixed-point fingerprint
    State state = State::Draft;
};

class PersistentIrStore {
public:
    /// CEP:WHAT: Module identity: FNV-1 over every method's bytecode in method-table order.
    /// CEP:WHY: The UGB module hash binds the bytecode version (ugb.md); identity must be content-addressed (Rule 5), never pointer- or string-based.
    /// CEP:STATUS: complete
    /// CEP:FAILURE: none (pure function).
    /// CEP:ASSUMES: the module's method table is stable for the lifetime of the store entry.
    /// CEP:COST: @cold. O(total bytecode bytes) per call; the compile path calls it twice per compile (key + version) on modules of at most a few KB.
    /// CEP:EVIDENCE: j4_persistent_ir_reuse_is_bit_identical (same content -> same key -> reuse).
    static uint64_t module_identity(const ugb::UGBModule& module) noexcept;

    /// CEP:WHAT: The method's bytecode version hash: FNV-1 over header words (id, register count, code length) and the code bytes.
    /// CEP:WHY: The reuse rule compares exactly this hash - any mismatch invalidates, so the store never guesses staleness (spec 12.1).
    /// CEP:STATUS: complete
    /// CEP:FAILURE: Returns 0 for an out-of-range method id (a value no real method hashes to; the caller's reuse check then fails closed).
    /// CEP:ASSUMES: FNV-64 collision risk is negligible for per-engine method sets (Rule 5 protocol constants).
    /// CEP:COST: @cold. O(code bytes) per call - one method, at most a few KB.
    /// CEP:EVIDENCE: j4_persistent_ir_invalidation_is_content_addressed (one byte change flips the hash).
    static uint64_t method_version_hash(const ugb::UGBModule& module,
                                        uint32_t method_id) noexcept;

    /// CEP:WHAT: The store key: module identity mixed with the method id.
    /// CEP:WHY: One 64-bit key (Rule 5) so lookups stay keyed - the store contains no enumeration (Rule 16).
    /// CEP:STATUS: complete
    /// CEP:FAILURE: none (pure function).
    /// CEP:ASSUMES: fnv1a_mix is the shared hash protocol (support/hash.hpp).
    /// CEP:COST: @cold. Two hash mixes per call.
    /// CEP:EVIDENCE: exercised on every store-path compile in tests/test_j4.cpp.
    static uint64_t store_key(const ugb::UGBModule& module,
                              uint32_t method_id) noexcept;

    /// CEP:WHAT: Returns the reusable entry for `key`, or nullptr.
    /// CEP:WHY: Reuse must be exact: present, hash-identical, not invalidated - anything else is dropped so the caller rebuilds (spec 12.1: replace, never patch).
    /// CEP:STATUS: complete
    /// CEP:FAILURE: Returns nullptr for absent/stale/invalidated entries - the caller rebuilds; no error path.
    /// CEP:ASSUMES: the caller moves the graph out of the returned entry and returns it via put() (the worker's session protocol).
    /// CEP:COST: @cold. Linear match over <= dozens of entries with a 16-byte header compare per entry; drops the stale entry in place (one erase).
    /// CEP:EVIDENCE: j4_persistent_ir_invalidation_is_content_addressed (stale drop + invalidate paths).
    PersistentIrEntry* find_reusable(uint64_t key,
                                     uint64_t bytecode_version_hash);

    /// CEP:WHAT: Stores (replaces) the entry for its key.
    /// CEP:WHY: The compile/worker returns the graph state to the store so the NEXT recompile starts from it (spec 12.1).
    /// CEP:STATUS: complete
    /// CEP:FAILURE: none (move-assign or push_back; OOM escalates per EXC-001).
    /// CEP:ASSUMES: entry.key is honored (set here from the key parameter).
    /// CEP:COST: @cold. One linear match + one move-assign/push_back per publish or compile.
    /// CEP:EVIDENCE: j4_persistent_ir_reuse_is_bit_identical (store.size() transitions).
    void put(uint64_t key, PersistentIrEntry entry);

    /// CEP:WHAT: Marks the entry invalidated (Rule 15: compilation is cancellable on invalidation).
    /// CEP:WHY: The dependency engine (or the caller) retires a method's state without freeing the graph mid-flight - the next find_reusable drops it.
    /// CEP:STATUS: complete
    /// CEP:FAILURE: Missing keys are a no-op - the caller owns the dependency-callback contract; no hidden error.
    /// CEP:ASSUMES: the invalidation decision belongs to the caller (the store only records it).
    /// CEP:COST: @cold. One linear match + one byte store.
    /// CEP:EVIDENCE: j4_persistent_ir_invalidation_is_content_addressed (invalidate then find_reusable == null).
    void invalidate(uint64_t key);

    size_t size() const noexcept { return entries_.size(); }

private:
    // A plain vector with keyed lookup: entries are large (a whole built
    // graph) and the store is @cold compile-time state, so the hot-path
    // word-packed FlatHashMap (Rule 50) is the WRONG container here — its
    // 32-byte slot bound exists for probe locality on hot paths. The store
    // holds at most one entry per J4-compiled method (dozens), so the
    // linear match is bounded and deliberate (Law 1: the cost is stated).
    std::vector<PersistentIrEntry> entries_;
};

}  // namespace vortex::j4
