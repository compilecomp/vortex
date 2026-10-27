// CEP:FILE: src/gc/collector.cpp
// CEP:WHAT: The ICGGC old-generation cycle: pause accounting, the
// published-code registry, and the phase machine (SATB concurrent mark,
// FinalMark with assertion-mode closure, CSet evacuation, root updates,
// sweep).
// CEP:WHY: docs/gc-icggc.md sections 8.4/8.5/8.6 — Rule 77's concurrent
// generational collection with pause bounding and Rule 120 telemetry.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: step()/maybe_start_cycle()/run_cycle() return named Result
// failures (SatbClosureViolated, OutOfMemory); Failed -> Idle on one-shot
// observation.
// CEP:ASSUMES: one mutator; the driver never overlaps cycles; published
// executables keep their GcCodeRecord alive.
// CEP:COST: @cold. Slices are config-bounded; STW pauses are bounded by
// roots/SATB tail, never heap size.
// CEP:EVIDENCE: tests/test_gc.cpp — gc_engine_phase_machine_steps_through_
// ladder, gc_satb_closure_* (DoD 2), gc_pause_p99_under_budget (DoD 1).
// ICGGC old-generation cycle (docs/gc-icggc.md section 8.4): the phase
// machine, SATB concurrent marking, CSet evacuation with Brooks forwarding,
// precise root updates, sweeping, and pacing. The Heap owns the region
// bookkeeping (src/gc/icggc.cpp); the engine orchestrates phases and slices.
#include "vortex/gc/collector.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "vortex/infra/ffi.hpp"
#include "vortex/support/log.hpp"

namespace vortex::gc {

// ---------------------------------------------------------------------------
// Pause accounting (docs/gc-icggc.md 8.5)
// ---------------------------------------------------------------------------

// CEP:WHAT: Maps a PauseKind to its stable observability name.
// CEP:WHY: Rule 120: pause events must be spellable, never guessed.
// CEP:STATUS: complete
// CEP:FAILURE: none (pure enum mapping).
// CEP:ASSUMES: consumers keep the strings stable.
// CEP:COST: @cold. One switch.
// CEP:EVIDENCE: the pause ring's dump path.
const char* pause_kind_name(PauseKind kind) noexcept {
    switch (kind) {
    case PauseKind::YoungGc: return "young_gc";
    case PauseKind::InitialMark: return "initial_mark";
    case PauseKind::FinalMark: return "final_mark";
    case PauseKind::UpdateRoots: return "update_roots";
    case PauseKind::MarkSlice: return "mark_slice";
    case PauseKind::EvacuateSlice: return "evacuate_slice";
    case PauseKind::SweepSlice: return "sweep_slice";
    }
    return "unknown";
}

// CEP:WHAT: Maps a PauseCause to its stable observability name.
// CEP:WHY: Rule 120: causes must be spellable, never guessed.
// CEP:STATUS: complete
// CEP:FAILURE: none (pure enum mapping).
// CEP:ASSUMES: consumers keep the strings stable.
// CEP:COST: @cold. One switch.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 15; the vx stats cause
//          breakdown.
const char* pause_cause_name(PauseCause cause) noexcept {
    switch (cause) {
    case PauseCause::Unspecified: return "unspecified";
    case PauseCause::AllocationSlowPath: return "allocation_slow_path";
    case PauseCause::OccupancyTrigger: return "occupancy_trigger";
    case PauseCause::ExplicitRequest: return "explicit_request";
    }
    return "unknown";
}

namespace {
uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

// CEP:WHAT: RAII pause accounting, legacy overload (cause = Unspecified).
// CEP:WHY: The M5 call-shape stays compiling while the cause labels land
//          (docs/infrastructure/06-observability.md section 6).
// CEP:STATUS: complete
// CEP:FAILURE: none (the destructor always records).
// CEP:ASSUMES: one PauseTimer live per pause scope; GcStats outlives it.
// CEP:COST: @cold. One steady_clock read at construct.
// CEP:EVIDENCE: the legacy record_pause overload tests.
PauseTimer::PauseTimer(GcStats& stats, PauseKind kind)
    : stats_(stats), kind_(kind), cause_(PauseCause::Unspecified) {
    start_ns_ = now_ns();
}

// CEP:WHAT: RAII pause accounting: records the scope's ns duration into the stats ring.
// CEP:WHY: docs/gc-icggc.md 8.5 — slices and STW phases are both pause events.
// CEP:STATUS: complete
// CEP:FAILURE: none (the destructor always records).
// CEP:ASSUMES: one PauseTimer live per pause scope; GcStats outlives it.
// CEP:COST: @cold. One steady_clock read at construct + destruct.
// CEP:EVIDENCE: the DoD p99 gate (gc_pause_p99_under_budget) is computed from its records.
PauseTimer::PauseTimer(GcStats& stats, PauseKind kind, PauseCause cause)
    : stats_(stats), kind_(kind), cause_(cause) {
    start_ns_ = now_ns();
}

PauseTimer::~PauseTimer() {
    stats_.record_pause(kind_, now_ns() - start_ns_, cause_);
}

// CEP:WHAT: Records one pause event into the bounded ring.
// CEP:WHY: docs/gc-icggc.md 8.5 — the DoD p99 source (Rule 28: bounded buffers).
// CEP:STATUS: complete
// CEP:FAILURE: none (the ring clears at capacity).
// CEP:ASSUMES: single mutator.
// CEP:COST: @cold. One push.
// CEP:EVIDENCE: gc_pause_p99_under_budget_on_allocation_stress.
void GcStats::record_pause(PauseKind kind, uint64_t duration_ns,
                           PauseCause cause) {
    if (pauses.size() >= kMaxPauseEvents) pauses.clear();  // bounded ring
    pauses.push_back(PauseEvent{duration_ns, kind, cause});
}

// CEP:WHAT: Nearest-rank percentile over the recorded pause ring.
// CEP:WHY: the DoD's p99 gate.
// CEP:STATUS: complete
// CEP:FAILURE: 0 for an empty ring.
// CEP:ASSUMES: the ring is stable during the call (single mutator).
// CEP:COST: @cold. O(n log n) over <= 4096 events.
// CEP:EVIDENCE: the DoD test.
uint64_t GcStats::pause_percentile_ns(unsigned pct) const {
    if (pauses.empty()) return 0;
    std::vector<uint64_t> durations;
    durations.reserve(pauses.size());
    for (const PauseEvent& e : pauses) durations.push_back(e.duration_ns);
    std::sort(durations.begin(), durations.end());
    // Nearest-rank percentile.
    const size_t rank =
        (static_cast<size_t>(pct) * durations.size() + 99) / 100;
    return durations[std::min(rank, durations.size()) - 1];
}

// CEP:WHAT: Maximum recorded pause duration.
// CEP:WHY: diagnostics alongside the percentile.
// CEP:STATUS: complete
// CEP:FAILURE: 0 for an empty ring.
// CEP:ASSUMES: single mutator.
// CEP:COST: @cold. O(n).
// CEP:EVIDENCE: diagnostic consumers.
uint64_t GcStats::max_pause_ns() const {
    uint64_t m = 0;
    for (const PauseEvent& e : pauses) m = std::max(m, e.duration_ns);
    return m;
}

// ---------------------------------------------------------------------------
// GcCodeRegistry (docs/gc-icggc.md 8.6)
// ---------------------------------------------------------------------------

// CEP:WHAT: Registers one published executable's GC record.
// CEP:WHY: docs/gc-icggc.md 8.6 — the JIT metadata interchange.
// CEP:STATUS: complete
// CEP:FAILURE: none (the weak entry expires with the executable).
// CEP:ASSUMES: the caller keeps the returned shared_ptr in the executable.
// CEP:COST: @cold (per publication). One push.
// CEP:EVIDENCE: publish_baseline / publish_j2.
std::shared_ptr<GcCodeRecord> GcCodeRegistry::register_code(
    const uint8_t* base, size_t bytes, GcCodeRecord::Kind kind,
    uint32_t register_count,
    std::shared_ptr<const std::vector<uint8_t>> gc_maps) {
    auto rec = std::make_shared<GcCodeRecord>();
    rec->code_base = base;
    rec->code_bytes = bytes;
    rec->kind = kind;
    rec->register_count = register_count;
    rec->gc_maps = std::move(gc_maps);
    entries_.push_back(rec);
    return rec;
}

// CEP:WHAT: Resolves a pc to its code record, pruning expired entries.
// CEP:WHY: the stack walk's frame-identity step.
// CEP:STATUS: complete
// CEP:FAILURE: nullptr outside every registered range (the C++ boundary).
// CEP:ASSUMES: single mutator; records outlive their code mappings.
// CEP:COST: @warm per frame. Linear over live records with amortized prune.
// CEP:EVIDENCE: gc_young_gc_inside_jit_allocation_keeps_parity (jit_frames_scanned).
const GcCodeRecord* GcCodeRegistry::lookup(uintptr_t pc) noexcept {
    const GcCodeRecord* found = nullptr;
    size_t write = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        auto rec = entries_[i].lock();
        if (!rec) continue;  // released executable: pruned
        entries_[write++] = entries_[i];
        const uintptr_t base = reinterpret_cast<uintptr_t>(rec->code_base);
        if (pc >= base && pc < base + rec->code_bytes) found = rec.get();
    }
    entries_.resize(write);
    return found;
}

// CEP:WHAT: Counts live (non-expired) registry entries.
// CEP:WHY: diagnostics for the registry's weak-ownership model.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: single mutator.
// CEP:COST: @cold. O(n).
// CEP:EVIDENCE: diagnostic consumers.
size_t GcCodeRegistry::live_count() const noexcept {
    size_t n = 0;
    for (const auto& w : entries_)
        if (!w.expired()) ++n;
    return n;
}

// CEP:WHAT: The process-wide registry accessor.
// CEP:WHY: one runtime per process (M0 contract).
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: single initialization.
// CEP:COST: @cold after first call (static init).
// CEP:EVIDENCE: both publish paths.
GcCodeRegistry& gc_code_registry() noexcept {
    static GcCodeRegistry registry;
    return registry;
}

// ---------------------------------------------------------------------------
// IcggcEngine
// ---------------------------------------------------------------------------

struct IcggcEngine::Impl {
    explicit Impl(Heap& h) : heap(h) {
        mark_stack.reserve(64);  // the trace's first roots land immediately
    }

    Heap& heap;
    // ConcurrentMark worklist (DFS stack; docs/gc-icggc.md 8.4).
    std::vector<HeapObject*> mark_stack;
    // Assertion-mode snapshot: the full old-space closure as of InitialMark
    // (docs/gc-icggc.md 8.4 — recorded eagerly inside the pause, before any
    // mutator store can interfere).
    std::vector<HeapObject*> snapshot_closure;
    // SATB values logged this cycle (moved out of the heap buffer at
    // FinalMark; the assertion-mode verifier traces them as edges).
    std::vector<uint64_t> cycle_satb;
    // Evacuate cursor over the selected CSet.
    std::vector<uint32_t> cset;
    size_t cset_cursor = 0;
    // Sweep worklist: every old region needs either reclaim or mark-clear.
    enum class SweepAction : uint8_t { Reclaim, ClearMarks, ReclaimHumongous };
    std::vector<std::pair<uint32_t, SweepAction>> sweep_queue;
    size_t sweep_cursor = 0;

// CEP:WHAT: Marks one object and queues it for tracing (dedup by the mark bit).
// CEP:WHY: the SATB trace's core (docs 8.4).
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: o is a valid body; live_bytes accrue to its region.
// CEP:COST: @warm. O(1).
// CEP:EVIDENCE: every old-cycle test.
    void mark_object(HeapObject* o) {
        if (o->header.mark) return;
        o->header.mark = 1;
        if (Region* r = heap.region_of(o)) r->live_bytes += o->header.size;
        mark_stack.push_back(o);
    }
    /// Marks + queues the tagged targets of one body. Old targets enter the
    /// live accounting; young targets trace as the root-region leg (docs/
    /// gc-icggc.md 8.4 — allocations during the cycle are implicitly live).
// CEP:WHAT: Marks + queues the tagged targets of one body.
// CEP:WHY: the concurrent trace's per-body work (young targets are the root-region leg).
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: for_each_tagged_slot's shape contract.
// CEP:COST: @warm. O(slots).
// CEP:EVIDENCE: the closure tests.
    void trace_body(HeapObject* o) {
        for_each_tagged_slot(o, heap.double_klass(), [&](TaggedValue* s) {
            if (!s->is_heap_object()) return;
            HeapObject* t = s->as_heap_object();
            if (t == nullptr) return;
            mark_object(t);
        });
    }
};

// CEP:WHAT: Constructs the engine bound to its heap.
// CEP:WHY: the Heap owns the engine instance.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: heap outlives the engine.
// CEP:COST: @cold. One pimpl allocation.
// CEP:EVIDENCE: the Heap constructor.
IcggcEngine::IcggcEngine(Heap& heap) : heap_(heap) {
    impl_ = std::make_unique<Impl>(heap);
}
// CEP:WHAT: Releases the engine's cycle state.
// CEP:WHY: the Heap owns the engine.
// CEP:STATUS: complete
// CEP:FAILURE: none.
// CEP:ASSUMES: no cycle is mid-flight at destruction.
// CEP:COST: @cold.
// CEP:EVIDENCE: the Fixture teardowns.
IcggcEngine::~IcggcEngine() = default;

// CEP:WHAT: Allocation-based old-space occupancy percent (the trigger's input).
// CEP:WHY: docs/gc-icggc.md 8.4 — live bytes are unknown between marks.
// CEP:STATUS: complete
// CEP:FAILURE: 0 when the old space is empty.
// CEP:ASSUMES: region tops are trustworthy.
// CEP:COST: @warm. O(regions).
// CEP:EVIDENCE: the stress test's trigger path.
uint32_t IcggcEngine::old_occupancy_pct() const noexcept {
    // Allocation-based occupancy (the trigger runs between marks — live
    // bytes are only known AFTER a cycle, so the trigger counts committed
    // region bytes: bump tops + humongous runs over the old-space size).
    uint64_t used = 0;
    for (const Region& r : heap_.regions()) {
        switch (r.state) {
        case RegionState::Bump:
        case RegionState::Occupied:
        case RegionState::CSet:
            used += static_cast<uint64_t>(r.top - r.start);
            break;
        case RegionState::HumongousStart:
            used += r.live_bytes != 0
                        ? r.live_bytes
                        : static_cast<uint64_t>(r.top - r.start);
            break;
        default:
            break;
        }
    }
    const uint64_t total = heap_.old_bytes();
    return static_cast<uint32_t>(total != 0 ? (used * 100) / total : 0);
}

// CEP:WHAT: Trigger check + the InitialMark pause (SATB flip, roots, snapshot).
// CEP:WHY: docs/gc-icggc.md 8.4 cycle start.
// CEP:STATUS: complete
// CEP:FAILURE: named failures from the root scan path ride through.
// CEP:ASSUMES: the driver calls it between mutator executions.
// CEP:COST: @cold. Pause ~ O(roots) (+ assertion-mode snapshot closure).
// CEP:EVIDENCE: gc_engine_phase_machine_steps_through_ladder.
support::Result<void> IcggcEngine::maybe_start_cycle() {
    if (phase_ != Phase::Idle) return support::ok();
    if (old_occupancy_pct() < static_cast<uint32_t>(config.old_gc_trigger_pct)) {
        return support::ok();
    }
    // The cycle starts: adopt the JIT's bump BEFORE the InitialMark walk
    // (the young-as-roots leg must see the true young top — but NEVER in
    // the idle early-return path: the stale context copy would rewind the
    // heap's top and every allocation would reuse recycled addresses).
    heap_.sync_tlab_from_jit();
    // Cycle start: the InitialMark pause (docs/gc-icggc.md 8.4). The cycle's
    // pause-ring cause: the ONLY start mechanism today is the occupancy
    // trigger (run_cycle routes through this check too) — ExplicitRequest is
    // the adaptive-initiation batch's reserved label
    // (docs/infrastructure/06-observability.md section 6).
    cycle_cause_ = PauseCause::OccupancyTrigger;
    Impl& im = *impl_;
    {
        PauseTimer pause(stats_, PauseKind::InitialMark, cycle_cause_);
        heap_.set_marking_active(true);  // the SATB flip
        heap_.satb_buffer().clear();
        im.cycle_satb.clear();
        im.snapshot_closure.clear();
        for (Region& r : heap_.regions_mut()) r.live_bytes = 0;
        RootSet roots;
        scan_runtime_roots(heap_.gc_state(), roots, &stats_);
        for (TaggedValue* s : roots.tagged_slots) {
            if (s->is_heap_object()) im.mark_object(s->as_heap_object());
        }
        for (void** s : roots.raw_slots) {
            auto* t = static_cast<HeapObject*>(*s);
            if (t != nullptr && heap_.is_heap_addr(t)) im.mark_object(t);
        }
        for (HeapObject* p : roots.pinned) {
            im.mark_object(p);
            p->header.pinned = 1;  // named state for the no-evacuate rule
        }
        // Young-as-roots: every young object is live; their fields seed the
        // old trace. The mark bits are cleared HERE (inside the pause):
        // consecutive cycles with no young GC in between would otherwise
        // skip every young body via mark_object's dedup and lose every
        // young->old edge (the 14b-A review's probe-proven blocker).
        heap_.for_each_young_object([&im](HeapObject* o) {
            if (o->header.size == 0) return;
            o->header.mark = 0;
            im.mark_object(o);
        });
        // Assertion-mode snapshot: the full old-space closure as of NOW,
        // recorded by a PRIVATE BFS with its own visited set (the concurrent
        // protocol's mark bits stay untouched, so ConcurrentMark still runs
        // as slices while the mutator stores — the closure only defines
        // what FinalMark verifies). Single mutator: no interference inside
        // the pause.
        if (config.assertion_mode) {
            std::vector<HeapObject*> visited;
            auto seen = [&visited](HeapObject* o) {
                return std::binary_search(visited.begin(), visited.end(), o);
            };
            std::vector<HeapObject*> frontier;
            for (TaggedValue* s : roots.tagged_slots) {
                if (s->is_heap_object()) frontier.push_back(s->as_heap_object());
            }
            for (void** s : roots.raw_slots) {
                auto* t = static_cast<HeapObject*>(*s);
                if (t != nullptr) frontier.push_back(t);
            }
            for (HeapObject* p : roots.pinned) frontier.push_back(p);
            heap_.for_each_young_object(
                [&frontier](HeapObject* o) { frontier.push_back(o); });
            while (!frontier.empty()) {
                HeapObject* o = frontier.back();
                frontier.pop_back();
                if (o == nullptr || !heap_.is_heap_addr(o) || seen(o)) continue;
                visited.push_back(o);
                std::sort(visited.begin(), visited.end());
                if (heap_.region_of(o) != nullptr) {
                    im.snapshot_closure.push_back(o);
                }
                for_each_tagged_slot(o, heap_.double_klass(),
                                     [&](TaggedValue* s) {
                                         if (s->is_heap_object()) {
                                             frontier.push_back(
                                                 s->as_heap_object());
                                         }
                                     });
            }
            stats_.closure_checks++;
        }
    }
    stats_.old_cycles++;
    phase_ = Phase::ConcurrentMark;
    return support::ok();
}

// CEP:WHAT: Advances one phase transition or bounded slice (the stepwise contract).
// CEP:WHY: docs/gc-icggc.md 8.4 — the mutator runs between steps.
// CEP:STATUS: complete
// CEP:FAILURE: named SatbClosureViolated (assertion mode) and OutOfMemory (evacuation).
// CEP:ASSUMES: no guest code runs inside a step.
// CEP:COST: @cold. Slice work is config-bounded.
// CEP:EVIDENCE: the phase-machine and closure tests.
support::Result<IcggcEngine::Phase> IcggcEngine::step() {
    Impl& im = *impl_;
    // Every phase reads heap state the mutator's JIT bump may have advanced
    // (young-as-roots at InitialMark, evacuate's phase-4 young walk).
    heap_.sync_tlab_from_jit();
    switch (phase_) {
    case Phase::Idle:
        return phase_;

    case Phase::ConcurrentMark: {
        // One bounded slice (docs/gc-icggc.md 8.5): mark_queue pops.
        {
            PauseTimer pause(stats_, PauseKind::MarkSlice, cycle_cause_);
            uint32_t popped = 0;
            while (!im.mark_stack.empty() &&
                   popped < config.mark_slice_items) {
                HeapObject* o = im.mark_stack.back();
                im.mark_stack.pop_back();
                im.trace_body(o);
                stats_.mark_queue_pops++;
                ++popped;
            }
            stats_.slice_steps++;
        }
        if (im.mark_stack.empty()) phase_ = Phase::FinalMark;
        return phase_;
    }

    case Phase::FinalMark: {
        PauseTimer pause(stats_, PauseKind::FinalMark, cycle_cause_);
        // Drain the SATB log: every overwritten reference the mutator
        // captured during ConcurrentMark joins the trace (the tail).
        im.cycle_satb = std::move(heap_.satb_buffer());
        heap_.satb_buffer().clear();
        stats_.satb_logs += im.cycle_satb.size();
        uint64_t tail = 0;
        for (uint64_t word : im.cycle_satb) {
            const TaggedValue v = TaggedValue::from_raw(word);
            if (!v.is_heap_object()) continue;
            HeapObject* t = v.as_heap_object();
            if (!heap_.is_heap_addr(t) || t->header.mark) continue;
            im.mark_object(t);
            ++tail;
            // Drain the whole tail inside this pause (bounded by the SATB
            // log, not the heap — docs/gc-icggc.md 8.4).
            while (!im.mark_stack.empty()) {
                HeapObject* o = im.mark_stack.back();
                im.mark_stack.pop_back();
                im.trace_body(o);
            }
        }
        stats_.satb_tail_entries += tail;
        heap_.set_marking_active(false);  // the snapshot is sealed
        // Assertion mode: the SATB closure gate (docs/gc-icggc.md 8.4).
        if (config.assertion_mode) {
            for (HeapObject* o : im.snapshot_closure) {
                if (!o->header.mark) {
                    phase_ = Phase::Failed;
                    return support::fail(
                        support::ErrorCode::RuntimeError,
                        "SatbClosureViolated: a snapshot-reachable object "
                        "stayed white (a missed SATB log, stale mark bits, "
                        "or an untraced edge — audit the cycle's legs)");
                }
            }
        }
        // Weak-handle decisions (Rule 85): unmarked old targets clear.
        if (heap_.gc_state().handles != nullptr) {
            auto* table =
                static_cast<infra::HandleTable*>(heap_.gc_state().handles);
            table->for_each_entry([&](uint32_t, infra::HandleTable::HandleEntry& e) {
                if (!e.alive || e.scope != infra::HandleScope::Weak ||
                    e.object == nullptr) {
                    return;
                }
                auto* t = static_cast<HeapObject*>(e.object);
                if (heap_.is_heap_addr(t) && heap_.region_of(t) != nullptr &&
                    !t->header.mark) {
                    e.object = nullptr;  // resolve() now returns nullptr
                    stats_.weak_handles_cleared++;
                }
            });
        }
        // CSet selection: Occupied regions below the live-ratio threshold,
        // excluding pinned regions (docs/gc-icggc.md 8.6). Fully-dead
        // regions reclaim directly. The current promotion target never
        // evacuates (promoted bodies land there unmarked — a CSet region
        // must not be an allocation target).
        im.cset.clear();
        im.sweep_queue.clear();
        std::vector<uint32_t> pinned_regions;
        for (void* pr : heap_.gc_state().pin_regions) {
            auto* region = static_cast<infra::PinningRegion*>(pr);
            for (void* p : region->pinned()) {
                if (p == nullptr) continue;
                const Region* r = heap_.region_of(p);
                if (r != nullptr) pinned_regions.push_back(r->index);
            }
        }
        const uint64_t cset_max =
            Region::kSize * config.evacuate_live_ratio_pct / 100;
        for (Region& r : heap_.regions_mut()) {
            switch (r.state) {
            case RegionState::Occupied: {
                const bool pinned = std::find(pinned_regions.begin(),
                                              pinned_regions.end(),
                                              r.index) != pinned_regions.end();
                if (r.live_bytes == 0) {
                    im.sweep_queue.emplace_back(
                        r.index, Impl::SweepAction::Reclaim);
                } else if (r.live_bytes <= cset_max && !pinned) {
                    r.state = RegionState::CSet;
                    im.cset.push_back(r.index);
                } else {
                    im.sweep_queue.emplace_back(
                        r.index, Impl::SweepAction::ClearMarks);
                }
                break;
            }
            case RegionState::Bump:
                im.sweep_queue.emplace_back(r.index,
                                            Impl::SweepAction::ClearMarks);
                break;
            case RegionState::HumongousStart:
                im.sweep_queue.emplace_back(
                    r.index,
                    r.live_bytes == 0 ? Impl::SweepAction::ReclaimHumongous
                                      : Impl::SweepAction::ClearMarks);
                break;
            case RegionState::HumongousCont:
                break;  // handled through its head
            case RegionState::CSet:
            case RegionState::Free:
                break;
            }
        }
        im.cset_cursor = 0;
        im.sweep_cursor = 0;
        phase_ = Phase::Evacuate;
        return phase_;
    }

    case Phase::Evacuate: {
        if (im.cset_cursor >= im.cset.size()) {
            phase_ = Phase::UpdateRoots;
            return phase_;
        }
        {
            PauseTimer pause(stats_, PauseKind::EvacuateSlice, cycle_cause_);
            for (uint32_t n = 0;
                 n < config.evacuate_regions_per_slice &&
                 im.cset_cursor < im.cset.size();
                 ++n) {
                Region& r = heap_.regions_mut()[im.cset[im.cset_cursor++]];
                auto evacuated = heap_.evacuate_region(r);
                if (!evacuated) {
                    phase_ = Phase::Failed;
                    heap_.set_marking_active(false);
                    return std::unexpected(evacuated.error());
                }
            }
            stats_.slice_steps++;
        }
        return phase_;
    }

    case Phase::UpdateRoots: {
        // The stale-oop update pause: every root slot resolves through
        // Brooks (docs/gc-icggc.md 8.4/8.6). Handles update too; weak
        // entries cleared at FinalMark carry nullptr and stay cleared.
        PauseTimer pause(stats_, PauseKind::UpdateRoots, cycle_cause_);
        RootSet roots;
        scan_runtime_roots(heap_.gc_state(), roots, &stats_);
        for (TaggedValue* s : roots.tagged_slots) {
            if (!s->is_heap_object()) continue;
            HeapObject* t = s->as_heap_object();
            if (heap_.is_heap_addr(t)) {
                *s = TaggedValue::heap_pointer(gc_resolve(t));
            }
        }
        for (void** s : roots.raw_slots) {
            auto* t = static_cast<HeapObject*>(*s);
            if (t != nullptr && heap_.is_heap_addr(t)) *s = gc_resolve(t);
        }
        for (void** s : roots.weak_slots) {
            auto* t = static_cast<HeapObject*>(*s);
            if (t != nullptr && heap_.is_heap_addr(t)) *s = gc_resolve(t);
        }
        phase_ = Phase::Sweep;
        return phase_;
    }

    case Phase::Sweep: {
        if (im.sweep_cursor >= im.sweep_queue.size()) {
            phase_ = Phase::Idle;
            return phase_;
        }
        {
            PauseTimer pause(stats_, PauseKind::SweepSlice, cycle_cause_);
            uint32_t done = 0;
            while (im.sweep_cursor < im.sweep_queue.size() &&
                   done < config.sweep_slice_regions) {
                const auto& [index, action] = im.sweep_queue[im.sweep_cursor++];
                Region& r = heap_.regions_mut()[index];
                switch (action) {
                case Impl::SweepAction::Reclaim:
                    heap_.reclaim_region(r);
                    break;
                case Impl::SweepAction::ReclaimHumongous:
                    heap_.reclaim_humongous_run(r);
                    break;
                case Impl::SweepAction::ClearMarks:
                    heap_.clear_region_marks(r);
                    break;
                }
                ++done;
            }
            stats_.slice_steps++;
        }
        return phase_;
    }

    case Phase::Failed: {
        // One-shot observation: the driver sees the error, the engine
        // returns to Idle (docs/gc-icggc.md 8.4).
        phase_ = Phase::Idle;
        return support::fail(support::ErrorCode::RuntimeError,
                             "the previous cycle failed; the engine is Idle");
    }
    }
    return phase_;
}

// CEP:WHAT: Runs the cycle to completion (driver convenience).
// CEP:WHY: tests/drivers that do not interleave mutator work.
// CEP:STATUS: complete
// CEP:FAILURE: propagates step() failures by name.
// CEP:ASSUMES: the caller accepts one multi-phase pause sequence.
// CEP:COST: @cold. Sum of the phases.
// CEP:EVIDENCE: drive_cycle in tests/test_gc.cpp.
support::Result<void> IcggcEngine::run_cycle() {
    auto started = maybe_start_cycle();
    if (!started) return started;
    while (phase_ != Phase::Idle) {
        auto s = step();
        if (!s) return std::unexpected(s.error());
    }
    return support::ok();
}

}  // namespace vortex::gc
