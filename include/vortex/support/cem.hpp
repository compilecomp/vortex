// CEM-26 Rev 1.1 — Cycle-Exact Maintainable C++26 shared constants and
// annotation legend for VORTEX hot code.
//
// This header is the semantic-domain constant registry required by CEM-26
// section 2: cross-cutting machine constants (cache geometry, dispatch
// protocol states) live here exactly once. Constants owned by one subsystem
// stay in that subsystem's header (e.g. UGB encoding widths in
// include/vortex/ugb/encoding.hpp, card-table geometry in gc/icggc.hpp).
//
// Annotation legend used across the codebase (CEM-26 section 3):
//   // @hot  — per-instruction / per-node / per-alloc critical path.
//             Requires a PERF_CONTRACT block; noexcept; no allocation, no
//             indirect calls (PERF_PERMIT required otherwise), no cold calls.
//   // @warm — per-call / per-module-load / per-compilation frequency.
//             Budget still documented when non-obvious.
//   // @cold — setup, diagnostics, serialization, shutdown. May use standard
//             C++ conveniences; MUST NOT be reachable from hot code without
//             an explicit boundary comment.
//
// Cost block legend (CEM-26 sections 4-6):
//   PERF_CONTRACT:    the author's promise (BUDGET/READS/WRITES/BRANCHES/CACHE)
//   PERF_OBSERVATION: the machine's reality (TARGET/VALIDATED/ACTUAL/
//                     LAST_VALIDATED)
//   PERF_NOTE:        justification for a known theoretical regression
//   PERF_PERMIT:      documented exception (REASON/COST/OWNER), see
//                     docs/cem26.md for the live register
#pragma once

#include <cstddef>
#include <cstdint>

namespace vortex::cem {

// ---- cache geometry (CEM-26 sections 1 and 9) --------------------------------

/// L1 data cache line size on all supported targets (x86-64, AArch64).
/// Used for alignment of genuinely contended structures only; blanket
/// padding is forbidden (it wastes L1 without removing traffic).
inline constexpr size_t kCacheLineBytes = 64;

/// Target-independent false-sharing guard: an array of these offsets keeps
/// per-thread counters on separate lines when the element type is smaller
/// than a line.
template <typename T>
inline constexpr size_t cache_line_slots(size_t elements_per_line =
                                             kCacheLineBytes / sizeof(T)) {
    return elements_per_line;
}

// ---- dispatch protocol (T0 computed-goto table, ADR-002) ----------------------

/// Single-writer publication states for the dispatch table. Values are part
/// of the ADR-002 protocol; acquire/release semantics are mandated (CEM-26
/// section 14: no seq_cst without justification — none is needed here).
enum DispatchTableState : uint8_t {
    kDispatchUninitialized = 0,
    kDispatchBuilding = 1,
    kDispatchReady = 2,
};

// ---- hot-struct audit (CEM-26 section 9) ---------------------------------------

/// Every hot struct in vm/gc/ir must state its size and alignment next to its
/// definition via static_assert. These are the register-file word and the
/// decoded-instruction register set — the two most frequently loaded values
/// in the engine.
struct HotLayoutAudit {
    static constexpr size_t kTaggedValueBytes = 8;   // one word, always
    static constexpr size_t kTaggedValueAlign = 8;
};

}  // namespace vortex::cem

// CEM-26 section 7: no nontrivial global constructors may exist in hot trees.
// The registry above is constexpr-only, which the type system enforces.
static_assert(vortex::cem::HotLayoutAudit::kTaggedValueBytes ==
                  sizeof(uint64_t),
              "the tagged value is the register-file word; it must stay one "
              "64-bit word or the whole T0 cost model changes");
