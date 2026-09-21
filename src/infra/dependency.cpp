#include "vortex/infra/dependency.hpp"

namespace vortex::infra {

void DependencyGraph::register_assumption(const Assumption& a,
                                          DependentRegionRef region) {
    edges_[make_key(a.kind, a.subject_id)].push_back(region);
    ++total_edges_;
}

InvalidationBatch DependencyGraph::collect_and_remove(Key key) {
    // Hot regions patch immediately and their edge is dropped; cold regions
    // are reported as lazy AND their edge is KEPT so activation can fire it
    // later (docs/infrastructure/01: lazy invalidation).
    InvalidationBatch batch;
    auto it = edges_.find(key);
    if (it == edges_.end()) return batch;
    std::vector<DependentRegionRef> kept;
    for (DependentRegionRef& r : it->second) {
        if (r.hot) {
            batch.immediate.push_back(r);
        } else {
            batch.lazy.push_back(r);
            kept.push_back(r);
        }
    }
    total_edges_ -= it->second.size() - kept.size();
    if (kept.empty()) {
        edges_.erase(it);
    } else {
        it->second = std::move(kept);
    }
    return batch;
}

InvalidationBatch DependencyGraph::invalidate_klass(uint32_t klass_token) {
    // A class-load/modify breaks KlassLeaf(klass) and (conservatively)
    // ShapeStable edges on shapes owned by the klass are handled by the
    // runtime separately.
    InvalidationBatch batch = collect_and_remove(
        make_key(AssumptionKind::KlassLeaf, klass_token));
    InvalidationBatch finals =
        collect_and_remove(make_key(AssumptionKind::MethodFinal, klass_token));
    batch.immediate.insert(batch.immediate.end(), finals.immediate.begin(),
                           finals.immediate.end());
    batch.lazy.insert(batch.lazy.end(), finals.lazy.begin(), finals.lazy.end());
    return batch;
}

InvalidationBatch DependencyGraph::invalidate_method(uint32_t method_token) {
    return collect_and_remove(make_key(AssumptionKind::MethodFinal, method_token));
}

InvalidationBatch DependencyGraph::invalidate_shape(uint32_t shape_token) {
    return collect_and_remove(make_key(AssumptionKind::ShapeStable, shape_token));
}

InvalidationBatch DependencyGraph::activate_region(uint32_t method_id,
                                                   uint32_t region_id) {
    // Lazy invalidation fires when a cold region turns hot: the runtime calls
    // this before entering the region. Regions are stored under their
    // assumptions; we rescan lazy lists for this region.
    InvalidationBatch batch;
    for (auto& [key, refs] : edges_) {
        (void)key;
        for (auto it = refs.begin(); it != refs.end();) {
            if (it->method_id == method_id && it->region_id == region_id &&
                !it->hot) {
                batch.immediate.push_back(*it);
                it = refs.erase(it);
                --total_edges_;
            } else {
                ++it;
            }
        }
    }
    return batch;
}

size_t DependencyGraph::sweep_methods(const std::vector<uint32_t>& method_ids) {
    size_t reclaimed = 0;
    for (auto& [key, refs] : edges_) {
        (void)key;
        for (auto it = refs.begin(); it != refs.end();) {
            bool match = false;
            for (uint32_t m : method_ids) {
                if (it->method_id == m) {
                    match = true;
                    break;
                }
            }
            if (match) {
                it = refs.erase(it);
                --total_edges_;
                ++reclaimed;
            } else {
                ++it;
            }
        }
    }
    return reclaimed;
}

}  // namespace vortex::infra
