// CEP:FILE: tools/bench/bench_tiers.cpp
// CEP:WHAT: The tier benchmark: T0/J1/J2/J3/J4 per-call cost and compile time on one deterministic workload (the M4 J3>J2 harness).
// CEP:WHY: The M3 DoD deferred the tuned measurement harness to M4; tier deltas need a fixed methodology to be comparable across runs.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: Prints named errors and exits 1 on any assemble/verify/compile/run failure; wrong results abort the bench (correctness before speed).
// CEP:ASSUMES: a steady clock is available; the workload fits every tier's compile domain (J1's refusal is reported, not fatal).
// CEP:COST: @cold tool. 5 compiles + (warmups+reps) x 5 tier runs of a 200-iteration loop; median-of-samples reporting, no floats in the measurement path.
// CEP:EVIDENCE: run output (tier table); docs/roadmap.md M4 - the tuned measurement harness.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include "vortex/gc/icggc.hpp"
#include "vortex/infra/code_range.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/j3/full_jit.hpp"
#include "vortex/j4/max_jit.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/verifier.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;
using namespace vortex::ugb;
using namespace vortex::j2;
using namespace vortex::j3;
using namespace vortex::j4;

namespace {

/// The workload: 200 iterations of a countdown loop whose body calls a
/// helper (2*v) and accumulates. Deterministic result: 2 * sum(1..200).
constexpr const char* kWorkload = R"(
.method helper(regs=4, args=1)
    Const.I64 v1, 2
    Mul.I64 v1, v0, v1
    Return v1
.end
.method main(regs=6, args=1)
    Const.I64 v1, 0
    Const.I64 v2, 1
loop:
    Const.I64 v4, 0
    Eq.I64 v5, v0, v4
    JumpTrue v5, done
    Call.Direct v3, v0, 1, helper
    Add.I64 v1, v1, v3
    Sub.I64 v0, v0, v2
    Jump loop
done:
    Return v1
.end
)";

constexpr int64_t kIterations = 200;
constexpr int64_t kExpected = 2 * (kIterations * (kIterations + 1) / 2);
constexpr uint32_t kWarmups = 20;
constexpr uint32_t kReps = 41;  // odd: the median is a real sample

/// CEP:WHAT: Assembles + verifies the benchmark workload module.
/// CEP:WHY: All tiers must execute the exact same bytecode for the numbers to be comparable.
/// CEP:STATUS: complete
/// CEP:FAILURE: Exits 1 with the named refusal on assemble/verify failure.
/// CEP:ASSUMES: tool-time only: one assemble + one verify per process.
/// CEP:COST: main() consumes it before any timing.
/// CEP:EVIDENCE: docs/roadmap.md M4; the printed tier table.

std::unique_ptr<UGBModule> make_module() {
    auto module = assemble_module(kWorkload);
    if (!module) {
        std::fprintf(stderr, "bench: assemble: %s\n",
                     module.error().message.c_str());
        return nullptr;
    }
    auto verified = verify_module(*module);
    if (!verified) {
        std::fprintf(stderr, "bench: verify: %s\n",
                     verified.error().message.c_str());
        return nullptr;
    }
    return std::make_unique<UGBModule>(std::move(*module));
}

/// CEP:WHAT: Steady-clock nanosecond sampler for the measurement loops.
/// CEP:WHY: The median needs per-call samples from one monotonic clock (Rule 22 keeps wall-clock out of POLICY; measurement is the tool's job).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (wraps std::chrono; the clock is the contract).
/// CEP:ASSUMES: steady_clock is monotonic per process.
/// CEP:COST: One clock read per sample, outside the measured region boundaries.
/// CEP:EVIDENCE: docs/roadmap.md M4; the printed tier table.

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

/// R per-call samples -> median (nth_element on a private copy).
/// CEP:WHAT: Median of per-call samples via nth_element on a private copy.
/// CEP:WHY: The median (odd sample count) is robust to scheduler noise spikes that would distort a mean.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (samples.size() >= 1 by construction).
/// CEP:ASSUMES: odd kReps so the median is a real sample.
/// CEP:COST: O(R) selection per tier report.
/// CEP:EVIDENCE: docs/roadmap.md M4; the printed tier table.

uint64_t median_ns(std::vector<uint64_t> samples) {
    const size_t mid = samples.size() / 2;
    std::nth_element(samples.begin(), samples.begin() + mid, samples.end());
    return samples[mid];
}

/// CEP:WHAT: One formatted tier report line.
/// CEP:WHY: Fixed-width output keeps bench runs diff-able across commits.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure printf).
/// CEP:ASSUMES: called once per tier.
/// CEP:COST: Constant work per call.
/// CEP:EVIDENCE: docs/roadmap.md M4; the printed tier table.

void report(const char* tier, uint64_t compile_ns, uint64_t per_call_ns) {
    std::printf("  %-3s compile=%8llu ns   per-call median=%9llu ns\n",
                tier, (unsigned long long)compile_ns,
                (unsigned long long)per_call_ns);
}

}  // namespace

/// CEP:WHAT: Drives the bench: T0 interpreter, J1 stencil (refusal reported), J2/J3/J4 compiles + published runs.
/// CEP:WHY: The tier table (plus J4 fixed-point telemetry) is the M4 payoff measurement and the Rule-17 no-cliff guardrail.
/// CEP:STATUS: complete
/// CEP:FAILURE: Exits 1 with named errors on any failure; a wrong result aborts before timing is reported.
/// CEP:ASSUMES: Each tier compiles once and runs (warmups + reps) times; the J1 stencil domain may refuse the workload (reported, stays on T0 per Rule 11).
/// CEP:COST: Compile: O(method) per tier. Runs: 61 per tier. All costs are stated, none hidden.
/// CEP:EVIDENCE: docs/roadmap.md M4; the printed tier table.

int main() {
    auto module = make_module();
    if (!module) return 1;
    const int64_t id = module->find_method("main");
    if (id < 0) {
        std::fprintf(stderr, "bench: no main method\n");
        return 1;
    }
    const std::vector<TaggedValue> args{TaggedValue::smi(kIterations)};

    std::printf("vortex tier bench: %lld iterations, %u warmups, %u reps\n",
                (long long)kIterations, kWarmups, kReps);

    // ---- T0 ------------------------------------------------------------------
    {
        gc::Heap heap;
        vm::Interpreter interp(heap);
        auto boot = interp.build_module_runtime(*module);
        if (!boot) {
            std::fprintf(stderr, "bench: boot: %s\n",
                         boot.error().message.c_str());
            return 1;
        }
        j1::J1Bindings bindings;
        if (!j1::make_j1_bindings(heap, *module, &interp, bindings)) {
            std::fprintf(stderr, "bench: bindings failed\n");
            return 1;
        }
        std::vector<uint64_t> samples;
        samples.reserve(kReps);
        for (uint32_t i = 0; i < kWarmups + kReps; ++i) {
            const uint64_t s = now_ns();
            auto run = interp.run(*module, "main", args);
            const uint64_t e = now_ns();
            if (!run) {
                std::fprintf(stderr, "bench: T0 run: %s\n",
                             run.error().message.c_str());
                return 1;
            }
            if (run->value.as_smi() != kExpected) {
                std::fprintf(stderr, "bench: T0 wrong result\n");
                return 1;
            }
            if (i >= kWarmups) samples.push_back(e - s);
        }
        report("T0", 0, median_ns(std::move(samples)));
    }

    // ---- J1 / J2 / J3 / J4: compile once (timed), then run published code.
    {
        gc::Heap heap;
        vm::Interpreter interp(heap);
        auto boot = interp.build_module_runtime(*module);
        if (!boot) {
            std::fprintf(stderr, "bench: boot: %s\n",
                         boot.error().message.c_str());
            return 1;
        }
        j1::J1Bindings bindings;
        auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
        if (!br) {
            std::fprintf(stderr, "bench: bindings: %s\n",
                         br.error().message.c_str());
            return 1;
        }
        ugb::UGBMethod& method =
            module->method_table[static_cast<size_t>(id)];
        method.ensure_runtime_tables(method.code.size() / 4 + 1);

        // J1 stencil
        {
            j1::StencilTable corpus;
            j1::BaselineJit jit(corpus);
            j1::BaselineJob job;
            job.module = module.get();
            job.method_id = static_cast<uint32_t>(id);
            const uint64_t c0 = now_ns();
            auto code = jit.compile(job);
            const uint64_t c1 = now_ns();
            if (!code) {
                // Rule 11/76 in action: a named refusal keeps the method on
                // the lower tier. Reported, not fatal — the workload uses a
                // call, which is outside J1's stencil domain.
                std::printf("  J1  refused after %llu ns: %s (stays on T0)\n\n",
                            (unsigned long long)(c1 - c0),
                            code.error().message.c_str());
            } else {
                auto ex =
                    j1::publish_baseline(*code, infra::global_code_range());
                if (!ex) {
                    std::fprintf(stderr, "bench: J1 publish\n");
                    return 1;
                }
                std::vector<uint64_t> samples;
                samples.reserve(kReps);
                for (uint32_t i = 0; i < kWarmups + kReps; ++i) {
                    TaggedValue ret = TaggedValue::undefined();
                    heap.sync_tlab_top(
                        static_cast<uint8_t*>(bindings.context.tlab_top));
                    const uint64_t s = now_ns();
                    const int64_t rc =
                        ex->entry(&bindings.context, args.data(),
                                  static_cast<uint32_t>(args.size()), &ret);
                    const uint64_t e = now_ns();
                    if (rc != 0 || ret.as_smi() != kExpected) {
                        std::fprintf(stderr, "bench: J1 run failed\n");
                        return 1;
                    }
                    if (i >= kWarmups) samples.push_back(e - s);
                }
                report("J1", c1 - c0, median_ns(std::move(samples)));
            }
        }

        // J2 fast optimizing
        {
            J2Job job;
            job.module = module.get();
            job.method_id = static_cast<uint32_t>(id);
            job.profiles = &method.profiles;
            job.ics = &method.ics;
            job.node_cap = 200'000;
            const uint64_t c0 = now_ns();
            auto code = compile_j2(job);
            const uint64_t c1 = now_ns();
            if (!code) {
                std::fprintf(stderr, "bench: J2 compile: %s\n",
                             code.error().message.c_str());
                return 1;
            }
            auto ex = publish_j2(*code, infra::global_code_range());
            if (!ex) {
                std::fprintf(stderr, "bench: J2 publish\n");
                return 1;
            }
            std::vector<uint64_t> samples;
            samples.reserve(kReps);
            for (uint32_t i = 0; i < kWarmups + kReps; ++i) {
                TaggedValue ret = TaggedValue::undefined();
                heap.sync_tlab_top(
                    static_cast<uint8_t*>(bindings.context.tlab_top));
                const uint64_t s = now_ns();
                const int64_t rc =
                    ex->entry(&bindings.context, args.data(),
                              static_cast<uint32_t>(args.size()), &ret);
                const uint64_t e = now_ns();
                if (rc != 0 || ret.as_smi() != kExpected) {
                    std::fprintf(stderr, "bench: J2 run failed\n");
                    return 1;
                }
                if (i >= kWarmups) samples.push_back(e - s);
            }
            report("J2", c1 - c0, median_ns(std::move(samples)));
        }

        // J3 full optimizing
        {
            J2Job job;
            job.module = module.get();
            job.method_id = static_cast<uint32_t>(id);
            job.profiles = &method.profiles;
            job.ics = &method.ics;
            job.node_cap = 200'000;
            const uint64_t c0 = now_ns();
            auto code = compile_j3(job);
            const uint64_t c1 = now_ns();
            if (!code) {
                std::fprintf(stderr, "bench: J3 compile: %s\n",
                             code.error().message.c_str());
                return 1;
            }
            J2Code storage = std::move(*code);
            auto ex = publish_j3(storage, storage.summary,
                                 infra::global_code_range());
            if (!ex) {
                std::fprintf(stderr, "bench: J3 publish\n");
                return 1;
            }
            J3Executable j3ex = std::move(*ex);
            std::vector<uint64_t> samples;
            samples.reserve(kReps);
            for (uint32_t i = 0; i < kWarmups + kReps; ++i) {
                const uint64_t s = now_ns();
                auto run = run_j3(j3ex, bindings, args, interp);
                const uint64_t e = now_ns();
                if (!run || run->as_smi() != kExpected) {
                    std::fprintf(stderr, "bench: J3 run failed\n");
                    return 1;
                }
                if (i >= kWarmups) samples.push_back(e - s);
            }
            report("J3", c1 - c0, median_ns(std::move(samples)));
        }

        // J4 deterministic peak (the fixed point on the same input)
        {
            J2Job job;
            job.module = module.get();
            job.method_id = static_cast<uint32_t>(id);
            job.profiles = &method.profiles;
            job.ics = &method.ics;
            job.node_cap = 200'000;
            const uint64_t c0 = now_ns();
            J4Stats stats;
            auto code = compile_j4(job, nullptr, &stats);
            const uint64_t c1 = now_ns();
            if (!code) {
                std::fprintf(stderr, "bench: J4 compile: %s\n",
                             code.error().message.c_str());
                return 1;
            }
            auto ex = publish_j2(*code, infra::global_code_range());
            if (!ex) {
                std::fprintf(stderr, "bench: J4 publish\n");
                return 1;
            }
            std::vector<uint64_t> samples;
            samples.reserve(kReps);
            for (uint32_t i = 0; i < kWarmups + kReps; ++i) {
                TaggedValue ret = TaggedValue::undefined();
                heap.sync_tlab_top(
                    static_cast<uint8_t*>(bindings.context.tlab_top));
                const uint64_t s = now_ns();
                const int64_t rc =
                    ex->entry(&bindings.context, args.data(),
                              static_cast<uint32_t>(args.size()), &ret);
                const uint64_t e = now_ns();
                if (rc != 0 || ret.as_smi() != kExpected) {
                    std::fprintf(stderr, "bench: J4 run failed\n");
                    return 1;
                }
                if (i >= kWarmups) samples.push_back(e - s);
            }
            report("J4", c1 - c0, median_ns(std::move(samples)));
            std::printf("  (J4 pipeline runs: %u, stable=%d)\n",
                        stats.pipeline_runs,
                        stats.fixed_point.stable ? 1 : 0);
        }
    }
    return 0;
}
