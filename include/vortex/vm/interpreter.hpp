// T0 — speculative register interpreter (docs/tier-t0.md).
//
// Register frames, tagged vregs, mono/poly/mega inline caches, per-PC profile
// slots, adaptive bytecode rewriting (typed <-> canonical), superinstruction
// bigram tracking, TLAB allocation fast paths, card-marking write barriers,
// safepoint polls, and tiering handoff hooks.
//
// Hot-path laws honored here:
//   Rule 5  — dispatch and caches are keyed by tokens/IDs, never strings.
//   Rule 50 — lookups use flat open-addressing tables or direct indexing;
//             no unordered_map, no linear scans, no string compares.
//   Rule 52 — per-call argument windows use the frame pool; no heap
//             allocation in the steady-state dispatch loop (Rule 67).
//   Rule 57 — the dispatch table is immutable after one-time init (ADR-002).
//   Rule 72 — every threshold is a named, documented config knob.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vortex/gc/icggc.hpp"
#include "vortex/runtime/tiering.hpp"
#include "vortex/support/containers.hpp"
#include "vortex/support/result.hpp"
#include "vortex/support/tagged_value.hpp"
#include "vortex/ugb/module.hpp"

namespace vortex::vm {

using support::Result;

/// Interpreter configuration. The tiering thresholds are forwarded to the
/// policy; the rewrite knobs implement docs/tier-t0.md section 6.
struct InterpreterConfig {
    TieringThresholds tiering{};
    /// Consecutive generic successes before rewriting canonical -> typed.
    uint32_t typed_rewrite_threshold = 16;
    /// Failure count after which a typed form is rewritten back to canonical.
    uint32_t generic_rewrite_threshold = 8;
    /// Bigram executions before the pair is flagged for superstencil promotion
    /// (J1 consumes this in M1).
    uint32_t superinstruction_threshold = 64;
    /// Guest call-depth limit. Named and configurable (Rule 72); exceeding it
    /// is a guest-visible runtime error, not an abort.
    uint32_t max_call_depth = 256;
    /// Upper bound on New.Array lengths accepted by T0 slow paths.
    uint32_t max_array_length = 1'000'000;
    bool enable_adaptive_rewriting = true;
    bool enable_profiling = true;
    bool trace_execution = false;
};

/// Execution statistics for the `vx stats` sink and tests.
struct InterpStats {
    uint64_t instructions_executed = 0;
    uint64_t typed_instructions_executed = 0;
    uint64_t generic_instructions_executed = 0;
    uint64_t calls = 0;
    uint64_t allocations = 0;
    uint64_t safepoint_polls = 0;
    uint64_t ic_hits = 0;
    uint64_t ic_misses = 0;
    uint32_t typed_rewrites = 0;     // canonical -> typed
    uint32_t generic_rewrites = 0;   // typed -> canonical (chronic failure)
    uint32_t osr_requests = 0;
    uint32_t max_call_depth = 0;
};

/// Observability for tier transitions (Rule 28: silent tier transitions are
/// forbidden). Records are appended to a bounded ring on the interpreter.
struct TierTransitionRecord {
    enum class Kind : uint8_t { Promote, Demote, OsrRequest, Fallback };
    uint32_t method_id = 0;
    uint32_t bytecode_pc = 0;
    Tier from = Tier::T0;
    Tier to = Tier::T0;
    Kind kind = Kind::OsrRequest;
    const char* reason = "";  // static string; never freed
};

struct RunResult {
    TaggedValue value;
    InterpStats stats;
};

/// Builtins available to guest code (CALL_BUILTIN).
struct Builtin {
    std::string name;
    TaggedValue (*fn)(std::span<const TaggedValue> args, void* user) = nullptr;
    void* user = nullptr;
};

/// The T0 interpreter. One instance executes any verified UGB module.
/// An instance is single-threaded (mutator contract); separate threads use
/// separate instances over a shared heap only through the M2+ handshake
/// protocol (docs/infrastructure/04-threading-suspension.md).
class Interpreter {
public:
    explicit Interpreter(gc::Heap& heap, InterpreterConfig config = {});

    /// Registers a builtin (name -> native fn). The assembler interns builtin
    /// tokens by name; resolution happens at first call and caches by token.
    void register_builtin(std::string_view name,
                          TaggedValue (*fn)(std::span<const TaggedValue>, void*),
                          void* user = nullptr);

    /// Runs `entry(args)` to completion. The module is non-const: T0's adaptive
    /// rewriter patches speculative opcodes in place (docs/tier-t0.md section 6).
    /// Capabilities are negotiated before execution (Rule 3): methods requiring
    /// unknown or unsupported capabilities are rejected safely.
    Result<RunResult> run(ugb::UGBModule& module, std::string_view entry,
                          std::span<const TaggedValue> args);

    const InterpStats& stats() const noexcept { return stats_; }
    const InterpreterConfig& config() const noexcept { return config_; }
    InterpreterConfig& config_mutable() noexcept { return config_; }
    void set_tiering_observer(TieringObserver* observer) {
        tiering_.set_observer(observer);
    }

    /// Rule 28: recent tier-transition events in chronological order
    /// (bounded; the oldest is dropped by shift when full).
    std::span<const TierTransitionRecord> tier_transitions() const noexcept {
        return {transitions_.data(), transitions_size_};
    }

    /// Safepoint integration hook (Rule 81): invoked by SAFEPOINT_POLL.
    /// The handshake manager installs the real poll in M2+; the default is a
    /// no-op so the poll site itself is already correct.
    void set_safepoint_hook(void (*hook)(void*), void* user) noexcept {
        safepoint_hook_ = hook;
        safepoint_hook_user_ = user;
    }

private:
    // Arithmetic slow-path result: distinct overflow vs type-error statuses
    // so guest-visible error messages are precise (Rule 110: numeric
    // semantics are exact, overflow is a defined trap).
    enum class ArithStatus : uint8_t { Ok, SmiOverflow, TypeError };
    struct ArithResult {
        TaggedValue value;
        ArithStatus status = ArithStatus::Ok;
    };
    // CEM-26 section 9: hot return values travel in registers; 16 bytes is
    // the largest return the handlers tolerate without spilling.
    static_assert(sizeof(ArithResult) == 16,
                  "ArithResult must stay 16 bytes (register-returned on the "
                  "hot arithmetic path)");

    Result<RunResult> execute(ugb::UGBModule& module,
                              ugb::UGBMethod& method,
                              std::span<const TaggedValue> args);

    // Slow-path helpers (called from the dispatch loop). @hot (CEM-26):
    // pure register arithmetic, noexcept, inline-sized.
    // PERF_CONTRACT: blocks of record live at the definitions in
    // src/vm/interpreter.cpp (generic_add/generic_sub/generic_mul/
    // generic_compare/as_double).
    ArithResult generic_add(TaggedValue a, TaggedValue b) const noexcept;
    ArithResult generic_sub(TaggedValue a, TaggedValue b) const noexcept;
    ArithResult generic_mul(TaggedValue a, TaggedValue b) const noexcept;
    ArithResult generic_compare(TaggedValue a, TaggedValue b,
                                ugb::Op cmp) const noexcept;

    // Rule 5: field access resolves through the (klass_id, field_token)
    // cache; string lookup is the cold fallback that fills the cache.
    // Returns false for "receiver bad / field unresolved" — a stored
    // undefined field value is NOT conflated with resolution failure
    // (ADR-005: no value doubles as an error sentinel).
    bool get_field_cached(TaggedValue obj, uint32_t field_token,
                          ugb::UGBModule& module, TaggedValue& out,
                          int32_t* resolved_slot = nullptr);
    bool set_field_cached(TaggedValue obj, TaggedValue value,
                          uint32_t field_token, ugb::UGBModule& module);

    // Rule 3: capability negotiation at load time.
    Result<void> check_capabilities(const ugb::UGBModule& module) const;

    // Builds klass handles + token->slot tables on first execution.
    Result<void> build_module_runtime(ugb::UGBModule& module);

    // Dispatch-loop helpers.
    void record_backedge(ugb::UGBModule& module, ugb::UGBMethod& method,
                         uint32_t pc);
    int32_t resolve_method(ugb::UGBModule& module, uint32_t token) const;
    int32_t resolve_builtin(ugb::UGBModule& module, uint32_t token) const;
    int32_t resolve_virtual(ugb::UGBModule& module, uint32_t klass_id,
                            uint32_t method_token) const;
    /// Rule 110: type test and value extraction are separate — no value
    /// doubles as a "not a double" sentinel (NaN must flow). @hot.
    bool as_double(TaggedValue v, double& out) const noexcept;

    // Adaptive rewriting (docs/tier-t0.md section 6).
    void maybe_rewrite_to_typed(ugb::UGBMethod& m, size_t pc,
                                ugb::Op canonical, ugb::Op typed);
    void maybe_rewrite_to_generic(ugb::UGBMethod& m, size_t pc,
                                  ugb::Op typed);

    void record_bigram(ugb::Op first, ugb::Op second);
    void record_transition(uint32_t method_id, uint32_t pc, Tier from, Tier to,
                           TierTransitionRecord::Kind kind,
                           const char* reason);

    gc::Heap& heap_;
    InterpreterConfig config_;
    TieringPolicy tiering_;
    InterpStats stats_;
    KlassRegistry registry_;  // klass objects for module class tokens
    std::vector<Builtin> builtins_;

    // Frame pool: one reusable register file per call depth. Each frame owns
    // a contiguous, stable buffer (a deque would be chunked and would break
    // contiguous argument windows); buffers are reused across calls, so the
    // steady-state dispatch loop performs no allocation (Rule 67) — growth
    // happens only on a new maximum depth or register count.
    struct Frame {
        std::vector<TaggedValue> regs;
    };
    std::vector<Frame> frames_;
    size_t frames_live_ = 0;

    // Superinstruction hints for J1 (Rule 50: flat open addressing).
    support::FlatHashMap<uint64_t, uint64_t> bigram_counts_;

    // Rule 28: bounded, chronologically ordered transition buffer.
    std::array<TierTransitionRecord, 64> transitions_{};
    uint32_t transitions_size_ = 0;

    void (*safepoint_hook_)(void*) = nullptr;
    void* safepoint_hook_user_ = nullptr;

    uint32_t call_depth_ = 0;
};

}  // namespace vortex::vm

// CEM-26 section 9: hot-struct layout audit. InterpStats is the `vx stats`
// sink accumulated per instruction; its size is a cache-traffic event (the
// counter block sits in L1 for the whole run; the final copy out is 80
// bytes, not a per-bump cost).
static_assert(sizeof(vortex::vm::InterpStats) == 80,
              "InterpStats layout drifted; every counter here is "
              "per-instruction hot-path state");
