// Infra 1 — Dependency & Invalidation System, the "CHA engine"
// (docs/infrastructure/01-dependency-invalidation.md).
//
// Speculative optimization (J3/J4) registers assumptions; when the guest loads
// classes or transitions shapes, the runtime broadcasts invalidation and the
// engine atomically patches exactly the dependent RBPD regions. Lazy
// invalidation defers cold-region patching; sweep reclaims metadata on unload.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vortex::infra {

enum class AssumptionKind : uint8_t {
    KlassLeaf,      // "Class X has no subclasses"
    MethodFinal,    // "Method Y is final / never overridden"
    ShapeStable,    // "Shape Z will not transition"
    NoProfilePollution,
};

struct Assumption {
    AssumptionKind kind = AssumptionKind::KlassLeaf;
    uint32_t subject_id = 0;  // klass / method / shape token
};

struct DependentRegionRef {
    uint32_t method_id = 0;
    uint32_t region_id = 0;   // deopt::RegionTable id
    bool hot = true;          // cold regions allow lazy invalidation
};

/// Invalidation batch: the regions affected by one broadcast, ready for
/// targeted atomic patching (JMP -> TRAP or deopt-trampoline redirect).
struct InvalidationBatch {
    std::vector<DependentRegionRef> immediate;  // patch now
    std::vector<DependentRegionRef> lazy;       // patch on next execution
};

class DependencyGraph {
public:
    /// Registers an assumption made by a compiled region.
    void register_assumption(const Assumption& a, DependentRegionRef region);

    /// Invalidation broadcast entry points. Each returns the affected regions
    /// and clears the invalidated edges.
    InvalidationBatch invalidate_klass(uint32_t klass_token);
    InvalidationBatch invalidate_method(uint32_t method_token);
    InvalidationBatch invalidate_shape(uint32_t shape_token);

    /// Promotes lazily-invalidated cold regions when they turn hot.
    InvalidationBatch activate_region(uint32_t method_id, uint32_t region_id);

    /// Metadata GC on class/module unload: drops all edges mentioning the
    /// swept methods (docs/infrastructure/01 section: metadata garbage
    /// collection). Returns the number of reclaimed region refs.
    size_t sweep_methods(const std::vector<uint32_t>& method_ids);

    size_t edge_count() const noexcept { return total_edges_; }

private:
    using Key = uint64_t;
    static Key make_key(AssumptionKind kind, uint32_t subject) noexcept {
        return (static_cast<uint64_t>(kind) << 32) | subject;
    }

    InvalidationBatch collect_and_remove(Key key);

    // assumption -> dependent regions
    std::unordered_map<Key, std::vector<DependentRegionRef>> edges_;
    size_t total_edges_ = 0;
};

}  // namespace vortex::infra
