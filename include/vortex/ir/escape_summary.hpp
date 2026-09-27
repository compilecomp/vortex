// XLEA — Cross-Language Escape Analysis: escape summaries
// (docs/xlea.md section 4).
//
// Every optimizing-tier compile (J3/J4) publishes one EscapeSummary per
// function; callers consume summaries instead of re-analyzing the callee.
// This makes cross-language escape analysis compositional across tiers
// and languages: a J4 caller inlines a J3 callee and reads its summary
// rather than re-deriving it.
//
// Status: spec surface (docs/roadmap.md M3/M4) — the data structure and
// its laws are fixed here so the SoN graph, the persistent IR store and
// the dependency graph can be built against one canonical shape.
#pragma once

#include <cstdint>
#include <vector>

#include "vortex/support/containers.hpp"

namespace vortex::ir {

/// Per-parameter escape classification (docs/xlea.md section 4.1).
enum class ParamEscape : uint8_t {
    NoEscape,    // not stored, not returned, not globalized
    ArgEscape,   // stored somewhere the callee cannot see
    Unknown,     // summary unavailable for this parameter
};

/// The escape summary of one compiled function body.
///
/// Laws (docs/xlea.md section 4.1):
///   Soundness   — NoEscape may only be claimed when the analysis saw
///                 the whole callee body; any un-inlinable (foreign)
///                 call inside forces ArgEscape or Unknown.
///   Identity    — graph_hash binds the summary to the exact compiled
///                 graph; a recompile invalidates published summaries
///                 (routed through the DependencyGraph, the same channel
///                 LDPT sites use).
///   Monotonicity— a summary may be weakened (NoEscape -> ArgEscape) on
///                 invalidation, never strengthened without a recompile.
struct EscapeSummary {
    uint32_t method_id = 0;
    uint32_t param_count = 0;
    std::vector<ParamEscape> params;   // indexed by parameter position
    bool returns_heap_allocation = false;  // return value freshly allocated
    bool captures_global = false;          // stores into globals/statics
    uint64_t graph_hash = 0;               // SoN graph hash described

    /// Classification for a caller's argument at `index`; out-of-range =
    /// Unknown (never a silent over-read).
    ParamEscape param(uint32_t index) const noexcept {
        return index < params.size() ? params[index] : ParamEscape::Unknown;
    }
};

/// The published-summary store: per method id, the summary for the
/// current graph hash. Weakening on invalidation is monotonic; a
/// strengthen requires publishing a new graph hash (a recompile).
///
/// Storage: ids index a dense vector (Rule 50 — the flat map carries only
/// the id->slot probe; summaries are aggregates, not hot-path granules).
class EscapeSummaryTable {
public:
    void publish(const EscapeSummary& summary) {
        const uint32_t* slot = index_.find(summary.method_id);
        if (slot != nullptr) {
            EscapeSummary& existing = summaries_[*slot];
            if (existing.graph_hash == summary.graph_hash) {
                // Same graph: monotonic weakening only.
                for (uint32_t i = 0;
                     i < summary.param_count && i < existing.params.size();
                     ++i) {
                    if (existing.params[i] == ParamEscape::ArgEscape) continue;
                    if (summary.params[i] == ParamEscape::ArgEscape) {
                        existing.params[i] = ParamEscape::ArgEscape;
                    }
                }
                // returns_heap_allocation is a property of the graph (not
                // a weakenable claim) — same graph, same value.
                existing.captures_global =
                    existing.captures_global || summary.captures_global;
                return;
            }
            existing = summary;  // new graph hash: re-registration
            return;
        }
        const uint32_t idx = static_cast<uint32_t>(summaries_.size());
        summaries_.push_back(summary);
        index_.insert(summary.method_id, idx);
    }

    /// Invalidates the summary of a recompiled method (the dependency
    /// graph routes this); the next publish re-registers it.
    void invalidate(uint32_t method_id) {
        const uint32_t* slot = index_.find(method_id);
        if (slot == nullptr) return;
        summaries_[*slot] = EscapeSummary{};  // weakened to empty (Unknown)
        index_.erase(method_id);
    }

    /// The summary for `method_id`, or nullptr when none is published
    /// (callers then treat every parameter as Unknown).
    const EscapeSummary* lookup(uint32_t method_id) const {
        const uint32_t* slot = index_.find(method_id);
        return slot != nullptr ? &summaries_[*slot] : nullptr;
    }

    size_t size() const noexcept { return index_.size(); }

private:
    std::vector<EscapeSummary> summaries_;
    support::FlatHashMap<uint32_t, uint32_t> index_;
};

}  // namespace vortex::ir
