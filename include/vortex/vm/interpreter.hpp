// T0 — speculative register interpreter (docs/tier-t0.md).
//
// Register frames, tagged vregs, mono/poly/mega inline caches, per-PC profile
// slots, adaptive bytecode rewriting (typed <-> canonical), superinstruction
// bigram tracking, TLAB allocation fast paths, card-marking write barriers,
// safepoint polls, and tiering handoff hooks.
#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vortex/gc/icggc.hpp"
#include "vortex/runtime/tiering.hpp"
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
class Interpreter {
public:
    explicit Interpreter(gc::Heap& heap, InterpreterConfig config = {});

    /// Registers a builtin (name -> native fn). The assembler interns builtin
    /// tokens by name; resolution happens at first call and caches in the IC.
    void register_builtin(std::string_view name,
                          TaggedValue (*fn)(std::span<const TaggedValue>, void*),
                          void* user = nullptr);

    /// Runs `entry(args)` to completion. The module is non-const: T0's adaptive
    /// rewriter patches speculative opcodes in place (docs/tier-t0.md section 6).
    Result<RunResult> run(ugb::UGBModule& module, std::string_view entry,
                          std::span<const TaggedValue> args);

    const InterpStats& stats() const noexcept { return stats_; }
    const InterpreterConfig& config() const noexcept { return config_; }
    InterpreterConfig& config_mutable() noexcept { return config_; }
    void set_tiering_observer(TieringObserver* observer) {
        tiering_.set_observer(observer);
    }

private:
    struct Frame;

    // Per-method interpreter caches (offset->index maps).
    struct MethodData {
        std::vector<uint32_t> instruction_offsets;  // byte offset per index
        std::vector<uint32_t> instruction_index;    // index per byte offset
        bool ready = false;
    };
    MethodData& method_data(const ugb::UGBMethod& m);
    void build_offset_maps(const ugb::UGBMethod& m, MethodData& md);

    Result<RunResult> execute(ugb::UGBModule& module,
                              ugb::UGBMethod& method,
                              std::span<const TaggedValue> args);

    // Slow-path helpers (called from the dispatch loop).
    TaggedValue generic_add(TaggedValue a, TaggedValue b);
    TaggedValue generic_sub(TaggedValue a, TaggedValue b);
    TaggedValue generic_mul(TaggedValue a, TaggedValue b);
    TaggedValue generic_compare(TaggedValue a, TaggedValue b, ugb::Op cmp);
    TaggedValue generic_get_field(TaggedValue obj, uint32_t field_token,
                                  const ugb::UGBModule& module);
    bool generic_set_field(TaggedValue obj, TaggedValue value,
                           uint32_t field_token, const ugb::UGBModule& module);

    // Runtime tables built lazily per module.
    struct ModuleData {
        std::vector<Klass*> klass_table;       // class token -> Klass*
        std::vector<int32_t> field_slot;       // field token -> slot index
        std::vector<int32_t> builtin_slot;     // builtin token -> builtins_ index
        std::vector<int32_t> method_resolution; // method token -> method_table idx
        bool ready = false;
    };
    ModuleData& module_data(ugb::UGBModule& module);
    Result<void> build_module_data(ugb::UGBModule& module, ModuleData& md);

    // Dispatch-loop helpers.
    void record_backedge(ugb::UGBModule& module, const ugb::UGBMethod& method);
    int32_t resolve_method(ugb::UGBModule& module, uint32_t token);
    int32_t resolve_builtin(ugb::UGBModule& module, uint32_t token);
    double as_double(TaggedValue v) const;

    // Adaptive rewriting (docs/tier-t0.md section 6).
    void maybe_rewrite_to_typed(const ugb::UGBMethod& m, size_t pc,
                                ugb::Op canonical, ugb::Op typed);
    void maybe_rewrite_to_generic(const ugb::UGBMethod& m, size_t pc,
                                  ugb::Op typed);

    void record_bigram(const ugb::UGBMethod& m, ugb::Op first, ugb::Op second);

    gc::Heap& heap_;
    InterpreterConfig config_;
    TieringPolicy tiering_;
    InterpStats stats_;
    KlassRegistry registry_;  // klass objects for module class tokens
    std::vector<Builtin> builtins_;
    // deque: references into these must stay valid across nested execute()
    // frames (a recursive call appends to them).
    std::deque<std::pair<const ugb::UGBMethod*, MethodData>> method_data_;
    std::deque<std::pair<ugb::UGBModule*, ModuleData>> module_data_;
    std::deque<std::pair<uint32_t, MethodHotness>> hotness_;
    std::unordered_map<uint64_t, uint64_t> bigram_counts_;  // superstencil hints
    uint32_t call_depth_ = 0;
};

}  // namespace vortex::vm
