// CEP:FILE: tests/test_threading.cpp
// CEP:WHAT: The M6-b threading/suspension DoD suite (docs/infrastructure/
//           04-threading-suspension.md section 10): polling-page state
//           machine + protection probe, T0 flag-first polls, J1 global
//           poll-word suspension parity, cross-thread handshake round trip
//           + bounded timeout, freeze_all latency with polling mutators,
//           M:N spawn/steal/yield/join/shutdown, cooperative freeze over
//           polling fibers, preemptive guard-page suspension within a
//           bounded budget, foreign-fault chaining, and the named rejects.
// CEP:WHY: Rule 121 discipline — every contract the milestone lands is
//           pinned by a regression test that fails loudly on drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: assertion failures count through the harness; no test
//           aborts the binary. Signal-based tests isolate faults into
//           forked children so the suite survives.
// CEP:ASSUMES: Linux x86-64 (mprotect/ucontext/sigaction); the process-wide
//           polling page, guard registry, and trap handler are shared —
//           every test restores the disarmed/disabled state on exit.
// CEP:COST: @cold. Test-only; no hot-path presence.
// CEP:EVIDENCE: this file is the evidence (DoD 1-14).
#include "vortex_test.hpp"

#include <errno.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "vortex/gc/icggc.hpp"
#include "vortex/infra/threading.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j1/stencil_corpus.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;
using namespace vortex::infra;
using namespace vortex::ugb;
using vortex::j1::StencilTable;
using Interpreter = vortex::vm::Interpreter;

namespace {

/// CEP:WHAT: Bounded busy-wait for a test predicate.
/// CEP:WHY: Rule 81 inside the tests: missed budgets FAIL, never hang.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — the predicate result is the failure surface.
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only spin loop.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
bool wait_for(const std::function<bool()>& pred,
              std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        sched_yield();
    }
    return pred();
}

// Restores the disarmed page state on scope exit (the page pair is
// process-wide; later tests and tiers must see a quiet word).
struct PageQuiet {
    ~PageQuiet() { (void)SafepointPollingPage::disarm(); }
};

// Restores the previous SIGSEGV disposition on scope exit.
struct TrapOff {
    bool armed = false;
    ~TrapOff() {
        if (armed) (void)GuardPageSuspender::disable_trap_suspension();
    }
};

/// CEP:WHAT: Assembles a UGB module from DSL text for the poll-loop fixtures.
/// CEP:WHY: Assembly failures surface as test assertions.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — assemble_module's own named rejects.
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
vm::Result<ugb::UGBModule> try_assemble(const char* src) {
    return assemble_module(src);
}

// A countdown loop with a safepoint poll per iteration: the J1/T0 poll
// consumers both run it; 5 iterations keeps every run bounded.
constexpr const char* kPollLoopSrc = R"(
.method f(regs=5, args=0)
    Const.I64 v1, 5
    Const.I64 v2, 1
loop:
    Safepoint
    Sub.I64 v1, v1, v2
    Const.I64 v3, 0
    Eq.I64 v4, v1, v3
    JumpTrue v4, done
    Jump loop
done:
    Return v1
.end
)";

/// CEP:WHAT: T0-side probe: runs the poll-loop method.
/// CEP:WHY: The T0 flag-first conversion is exercised through it.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — interp.run's named failures.
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
support::Result<vm::RunResult> run_t0_poll_loop(Interpreter& interp,
                                                ugb::UGBModule& module) {
    return interp.run(module, "f", {});
}

// J1 helper: bindings + compiled executable for kPollLoopSrc on a private
// heap/module pair (shape mirrors tests/test_j1.cpp).
struct J1Rig {
    std::optional<ugb::UGBModule> module;
    gc::Heap heap;
    vm::Interpreter interp;
    j1::J1Bindings bindings;
    StencilTable* corpus = nullptr;
    std::optional<j1::BaselineExecutable> ex;

/// CEP:WHAT: Constructs the J1 rig: heap, interpreter, print builtin.
/// CEP:WHY: The rig mirrors tests/test_j1.cpp's harness shape.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — construction cannot fail.
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
    explicit J1Rig(StencilTable* c)
        : corpus(c), interp(heap) {
        interp.register_builtin(
            "print", [](std::span<const TaggedValue>, void*) {
                return TaggedValue::undefined();
            });
    }

/// CEP:WHAT: Assembles + binds + compiles + publishes the poll-loop method.
/// CEP:WHY: Idempotent so the assertions can call it without double work.
/// CEP:STATUS: complete
/// CEP:FAILURE: false on any step's named failure (the assertions surface it).
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
    bool build() {
        if (ex.has_value()) return true;  // idempotent
        auto m = assemble_module(kPollLoopSrc);
        if (!m) return false;
        module = std::move(*m);
        auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
        if (!br) return false;
        j1::BaselineJit jit(*corpus);
        j1::BaselineJob job;
        job.module = &*module;
        job.method_id = static_cast<uint32_t>(module->find_method("f"));
        job.klass_addrs = &bindings.klass_addr_table;
        job.field_offsets = bindings.field_offsets.data();
        job.field_offset_count = bindings.field_offsets.size();
        job.double_klass = bindings.double_klass;
        auto code = jit.compile(job);
        if (!code) return false;
        auto published = j1::publish_baseline(*code);
        if (!published) return false;
        ex = std::move(*published);
        return true;
    }

/// CEP:WHAT: Runs the compiled method through run_baseline.
/// CEP:WHY: The rig's run is the J1-side parity probe.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — run_baseline's named failures.
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
    support::Result<TaggedValue> run() {
        return j1::run_baseline(*ex, bindings, {});
    }
};

}  // namespace

// ---- DoD 1: polling page state machine + protection probe ------------------

/// CEP:WHAT: DoD 1: the polling-page state machine + protection probe.
/// CEP:WHY: Every tier's suspension observable starts here; the probe proves the flip.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(polling_page_reserve_arm_disarm_cycle) {
    PageQuiet quiet;
    void* first = nullptr;
    auto r1 = SafepointPollingPage::reserve();
    VORTEX_EXPECT(r1.has_value());
    if (!r1) return;
    first = *r1;
    VORTEX_EXPECT(first != nullptr);

    // Idempotent reserve: the same base, the same word.
    auto r2 = SafepointPollingPage::reserve();
    VORTEX_EXPECT(r2.has_value() && *r2 == first);
    auto* word = SafepointPollingPage::poll_word();
    VORTEX_EXPECT(word != nullptr);
    VORTEX_EXPECT_EQ(word->load(std::memory_order_acquire), 0u);
    VORTEX_EXPECT(SafepointPollingPage::poll_word_raw() != nullptr);
    VORTEX_EXPECT(SafepointPollingPage::trap_page() != nullptr);

    auto st = SafepointPollingPage::state();
    VORTEX_EXPECT(!st.armed);
    VORTEX_EXPECT_EQ(static_cast<uint32_t>(st.kind),
                     static_cast<uint32_t>(SuspensionKind::None));

    // Disarm while disarmed: a no-op success.
    VORTEX_EXPECT(SafepointPollingPage::disarm().has_value());

    // Arm: the word turns nonzero with the kind encoded; the trap page
    // flips hostile (read() from /dev/zero into it fails with EFAULT —
    // read() always copies into the buffer, unlike /dev/null writes).
    const int devzero = open("/dev/zero", O_RDONLY);
    VORTEX_EXPECT(devzero >= 0);
    VORTEX_EXPECT(SafepointPollingPage::arm(SuspensionKind::Gc).has_value());
    st = SafepointPollingPage::state();
    VORTEX_EXPECT(st.armed);
    VORTEX_EXPECT_EQ(static_cast<uint32_t>(st.kind),
                     static_cast<uint32_t>(SuspensionKind::Gc));
    VORTEX_EXPECT(st.generation >= 1);
    VORTEX_EXPECT_EQ(word->load(std::memory_order_acquire), 0x8000'0001u);
    if (devzero >= 0) {
        VORTEX_EXPECT_EQ(
            static_cast<int>(read(devzero, SafepointPollingPage::trap_page(),
                                  1)),
            -1);
        VORTEX_EXPECT_EQ(errno, EFAULT);
    }

    // Disarm: the word clears before the page turns readable; the probe
    // succeeds again.
    VORTEX_EXPECT(SafepointPollingPage::disarm().has_value());
    st = SafepointPollingPage::state();
    VORTEX_EXPECT(!st.armed);
    VORTEX_EXPECT_EQ(word->load(std::memory_order_acquire), 0u);
    if (devzero >= 0) {
        VORTEX_EXPECT_EQ(
            static_cast<int>(read(devzero, SafepointPollingPage::trap_page(),
                                  1)),
            1);
        close(devzero);
    }

    // Re-arm bumps the generation.
    const uint64_t gen_before = SafepointPollingPage::state().generation;
    VORTEX_EXPECT(SafepointPollingPage::arm(SuspensionKind::Debugger).has_value());
    VORTEX_EXPECT(SafepointPollingPage::state().generation > gen_before);
    VORTEX_EXPECT_EQ(static_cast<uint32_t>(SafepointPollingPage::state().kind),
                     static_cast<uint32_t>(SuspensionKind::Debugger));
}

// ---- DoD 2: the armed word drives T0's slow path ---------------------------

/// CEP:WHAT: DoD 2: the armed word drives T0's slow path (hook fires, then clears).
/// CEP:WHY: The T0 flag-first conversion must reach the hook exactly on an armed word.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(polling_page_flag_visible_to_t0) {
    PageQuiet quiet;
    auto module = try_assemble(kPollLoopSrc);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    Interpreter interp(heap);

    // Disarmed baseline: no hook, no word — the poll is a counted no-op.
    auto base = run_t0_poll_loop(interp, *module);
    VORTEX_EXPECT(base.has_value());
    if (base) VORTEX_EXPECT_EQ(base->value.as_smi(), 0);

    // Armed word + hook that clears it: the FIRST poll fires the hook, the
    // re-check sees the cleared word, the loop completes.
    std::atomic<int> hook_fires{0};
    interp.set_safepoint_hook(
        [](void* user) {
            static_cast<std::atomic<int>*>(user)->fetch_add(1);
            (void)SafepointPollingPage::disarm();
        },
        &hook_fires);
    VORTEX_EXPECT(SafepointPollingPage::arm(SuspensionKind::Gc).has_value());
    auto r = run_t0_poll_loop(interp, *module);
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 0);
    VORTEX_EXPECT_EQ(hook_fires.load(), 1);
    VORTEX_EXPECT(!SafepointPollingPage::state().armed);
}

// ---- DoD 3: T0 poll-word binding default/override --------------------------

/// CEP:WHAT: DoD 3: the poll-word binding default/override/nullptr-reset.
/// CEP:WHY: The effective word is a documented selection contract.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(t0_safepoint_word_binding_default_and_override) {
    PageQuiet quiet;
    gc::Heap heap;
    Interpreter interp(heap);
    VORTEX_EXPECT(SafepointPollingPage::reserve().has_value());

    // Default: the global polling-page word.
    VORTEX_EXPECT(interp.effective_safepoint_word() ==
                  SafepointPollingPage::poll_word());

    // Explicit binding wins.
    std::atomic<uint32_t> local{0};
    interp.bind_safepoint_word(&local);
    VORTEX_EXPECT(interp.effective_safepoint_word() == &local);

    // nullptr resets to the global word.
    interp.bind_safepoint_word(nullptr);
    VORTEX_EXPECT(interp.effective_safepoint_word() ==
                  SafepointPollingPage::poll_word());

    // The override is observable end to end: arming the LOCAL word (not the
    // global one) fires the hook through the bound word.
    auto module = try_assemble(kPollLoopSrc);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    std::atomic<int> hook_fires{0};
    struct HookCtx {
        std::atomic<int>* fires;
        std::atomic<uint32_t>* word;
    } ctx{&hook_fires, &local};
    interp.set_safepoint_hook(
        [](void* user) {
            auto* c = static_cast<HookCtx*>(user);
            c->fires->fetch_add(1);
            c->word->store(0, std::memory_order_release);  // clear the BOUND word
        },
        &ctx);
    interp.bind_safepoint_word(&local);
    local.store(0x8000'0001u, std::memory_order_release);
    auto r = run_t0_poll_loop(interp, *module);
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT_EQ(r->value.as_smi(), 0);
    VORTEX_EXPECT_EQ(hook_fires.load(), 1);
    VORTEX_EXPECT_EQ(local.load(), 0u);  // the hook cleared the bound word
    interp.bind_safepoint_word(nullptr);
}

// ---- DoD 4: J1 observes the GLOBAL poll word --------------------------------

/// CEP:WHAT: DoD 4: J1 observes the GLOBAL poll word (armed -> deopt parity).
/// CEP:WHY: The J1 stencil binds the global word when the page is reserved.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(j1_global_poll_word_suspends) {
    PageQuiet quiet;
    // Reserve FIRST: the rig's J1 context then binds the GLOBAL poll word
    // deterministically (build-before-reserve would bind the per-executable
    // fallback word and the armed-global step would not reach the stencil).
    VORTEX_EXPECT(SafepointPollingPage::reserve().has_value());
    VORTEX_EXPECT(SafepointPollingPage::disarm().has_value());
    StencilTable corpus;
    j1::build_default_corpus(corpus);
    J1Rig rig(&corpus);
    const bool built = rig.build();
    VORTEX_EXPECT(built);
    if (!built) return;

    // Disarmed: the J1 loop runs to completion through the global word.
    auto ok_run = rig.run();
    if (!ok_run) {
        std::fprintf(stderr, "[j1] run#1 error: %s\n",
                     ok_run.error().message.c_str());
    }
    VORTEX_EXPECT(ok_run.has_value());
    if (ok_run) VORTEX_EXPECT_EQ(ok_run->as_smi(), 0);

    // Armed global word: the FIRST poll exits with the dedicated
    // safepoint-request id (M6-b review split: the poll no longer rides the
    // deopt id — a speculation deopt keeps its rerun contract; the armed
    // word names its own error and the arming caller owns the choreography).
    VORTEX_EXPECT(SafepointPollingPage::arm(SuspensionKind::Gc).has_value());
    auto armed_run = rig.run();
    VORTEX_EXPECT(!armed_run.has_value());
    if (!armed_run) {
        VORTEX_EXPECT(armed_run.error().message.find("safepoint request") !=
                      std::string::npos);
    }
    VORTEX_EXPECT(SafepointPollingPage::disarm().has_value());

    // Disarmed again: the loop completes (the executable still works).
    auto done_run = rig.run();
    VORTEX_EXPECT(done_run.has_value());
    if (done_run) VORTEX_EXPECT_EQ(done_run->as_smi(), 0);
}

// ---- DoD 5: handshake round trip across threads ------------------------------

/// CEP:WHAT: DoD 5: cross-thread request/acknowledge round trip + anonymous slot.
/// CEP:WHY: The LDPT flow depends on the anonymous-0 slot semantics.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(handshake_request_ack_roundtrip_cross_thread) {
    PageQuiet quiet;
    HandshakeManager hs;
    auto reg = hs.register_thread();
    VORTEX_EXPECT(reg.has_value());
    if (!reg) return;
    const uint64_t id = *reg;

    std::atomic<bool> requested{false};
    std::thread worker([&] {
        // The mutator side: ack as soon as the request lands, then wait to
        // be released before leaving (release clears the acked flag).
        while (!requested.load(std::memory_order_acquire)) sched_yield();
        VORTEX_EXPECT(hs.acknowledge(id).has_value());
        while (hs.pending(id)) sched_yield();
    });

    VORTEX_EXPECT(hs.request(SuspensionKind::Deopt, id).has_value());
    requested.store(true, std::memory_order_release);
    VORTEX_EXPECT(hs.pending(id));
    VORTEX_EXPECT(hs.wait_acknowledged(id, std::chrono::seconds(2)).has_value());
    VORTEX_EXPECT(hs.release(id).has_value());
    worker.join();
    VORTEX_EXPECT(hs.unregister_thread(id).has_value());
    VORTEX_EXPECT(!hs.pending(id));

    // Unknown ids reject by name.
    VORTEX_EXPECT(!hs.request(SuspensionKind::Gc, 9999).has_value());
    VORTEX_EXPECT(!hs.acknowledge(9999).has_value());
    VORTEX_EXPECT(!hs.unregister_thread(9999).has_value());
    // The anonymous slot (id 0) is permanent and accepts the LDPT flow.
    VORTEX_EXPECT(hs.request(SuspensionKind::Gc, 0).has_value());
    VORTEX_EXPECT(hs.pending(0));
    VORTEX_EXPECT(hs.acknowledge(0).has_value());
    VORTEX_EXPECT(!hs.unregister_thread(0).has_value());
}

// ---- DoD 6: bounded wait, named timeout --------------------------------------

/// CEP:WHAT: DoD 6: bounded wait -> HandshakeTimeout with measured latency.
/// CEP:WHY: Rule 81: a stuck thread fails the freeze loudly, never forever.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(handshake_wait_timeout_named_reject) {
    HandshakeManager hs;
    auto reg = hs.register_thread();
    VORTEX_EXPECT(reg.has_value());
    if (!reg) return;
    const uint64_t id = *reg;
    VORTEX_EXPECT(hs.request(SuspensionKind::Gc, id).has_value());

    const auto t0 = std::chrono::steady_clock::now();
    auto r = hs.wait_acknowledged(id, std::chrono::milliseconds(60));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    VORTEX_EXPECT(!r.has_value());
    if (!r) {
        VORTEX_EXPECT(r.error().message.find("timeout") != std::string::npos);
    }
    VORTEX_EXPECT(elapsed >= std::chrono::milliseconds(60));
    VORTEX_EXPECT_EQ(hs.stats().timeouts, static_cast<uint64_t>(1));
    (void)hs.unregister_thread(id);
}

// ---- DoD 7: freeze_all completes within budget over polling mutators -------

/// CEP:WHAT: DoD 7: freeze_all completes within budget over polling mutators.
/// CEP:WHY: The STW primitive is the substrate the GC background batch consumes.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(freeze_all_bounded_latency_with_polling_mutators) {
    PageQuiet quiet;
    HandshakeManager hs;
    VORTEX_EXPECT(SafepointPollingPage::reserve().has_value());
    VORTEX_EXPECT(SafepointPollingPage::disarm().has_value());
    auto* word = SafepointPollingPage::poll_word();

    // Two registered mutator threads poll the word in a loop: on an armed
    // word they acknowledge and park until the release.
    std::atomic<int> running{0};
    auto mutator = [&](uint64_t id) {
        running.fetch_add(1);
        while (!SafepointPollingPage::state().armed) sched_yield();
        (void)hs.acknowledge(id);
        while (SafepointPollingPage::state().armed) sched_yield();
        running.fetch_sub(1);
    };
    auto r1 = hs.register_thread();
    auto r2 = hs.register_thread();
    VORTEX_EXPECT(r1.has_value() && r2.has_value());
    if (!r1 || !r2) return;
    std::thread a(mutator, *r1);
    std::thread b(mutator, *r2);

    // The freeze must complete within the budget (both mutators ack) and
    // report its token.
    const auto t0 = std::chrono::steady_clock::now();
    auto frozen = hs.freeze_all(SuspensionKind::Gc, std::chrono::seconds(2));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    VORTEX_EXPECT(frozen.has_value());
    VORTEX_EXPECT(elapsed < std::chrono::seconds(1));
    VORTEX_EXPECT(hs.frozen());
    if (frozen) VORTEX_EXPECT(*frozen >= 1);

    // Registration is rejected for the whole freeze window (M6-b review M5):
    // a late registrant would run as a mutator inside the stopped world.
    auto late = hs.register_thread();
    VORTEX_EXPECT(!late.has_value());
    if (!late) {
        VORTEX_EXPECT(late.error().message.find("freeze window") !=
                      std::string::npos);
    }

    // A second freeze while frozen rejects by name.
    auto again = hs.freeze_all(SuspensionKind::Gc, std::chrono::seconds(1));
    VORTEX_EXPECT(!again.has_value());
    if (!again) {
        VORTEX_EXPECT(again.error().message.find("already frozen") !=
                      std::string::npos);
    }

    // The word is armed while frozen: the raw cooperative contract holds.
    VORTEX_EXPECT(SafepointPollingPage::state().armed);
    VORTEX_EXPECT(hs.thaw_all().has_value());
    VORTEX_EXPECT(!SafepointPollingPage::state().armed);
    VORTEX_EXPECT(!hs.frozen());

    a.join();
    b.join();
    (void)hs.unregister_thread(*r1);
    (void)hs.unregister_thread(*r2);
    VORTEX_EXPECT_EQ(hs.stats().freezes, static_cast<uint64_t>(1));
    (void)word;
}

// ---- DoD 8: M:N — more fibers than workers, stealing observed ---------------

namespace {
struct FiberWork {
    std::atomic<uint64_t>* counter;
    uint64_t spins;
};

/// CEP:WHAT: Fiber entry: bounded spin then count (M:N workloads).
/// CEP:WHY: Entries return Result; exceptions never cross fibers (Rule 114).
/// CEP:STATUS: complete
/// CEP:FAILURE: none — a trivial workload.
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
support::Result<void> counting_entry(void* user) {
    auto* w = static_cast<FiberWork*>(user);
    for (uint64_t i = 0; i < w->spins; ++i) sched_yield();
    w->counter->fetch_add(1, std::memory_order_release);
    return support::ok();
}

/// CEP:WHAT: Fiber entry: holds a worker for a fixed duration (steal forcing).
/// CEP:WHY: The blocking window makes the steal deterministic.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — a trivial workload.
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
support::Result<void> blocker_entry(void* user) {
    auto* spins = static_cast<uint64_t*>(user);
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(*spins);
    while (std::chrono::steady_clock::now() < until) sched_yield();
    return support::ok();
}
}  // namespace

/// CEP:WHAT: DoD 8: M:N fan-out (32 fibers / 2 workers) with deterministic stealing.
/// CEP:WHY: The scheduler must rebalance round-robin spawn placement.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(workstealing_spawns_more_fibers_than_workers) {
    WorkStealingPool pool(2);
    VORTEX_EXPECT_EQ(pool.os_thread_count(), 2u);

    std::atomic<uint64_t> counter{0};
    FiberWork work{&counter, 64};
    std::vector<uint64_t> ids;
    for (int i = 0; i < 32; ++i) {
        auto spawned = pool.spawn(counting_entry, &work);
        VORTEX_EXPECT(spawned.has_value());
        if (spawned) ids.push_back(*spawned);
    }
    VORTEX_EXPECT_EQ(ids.size(), size_t{32});
    VORTEX_EXPECT_EQ(pool.fiber_count(), static_cast<uint64_t>(32));
    for (uint64_t id : ids) {
        auto j = pool.join(id);
        VORTEX_EXPECT(j.has_value());
    }
    VORTEX_EXPECT_EQ(counter.load(), static_cast<uint64_t>(32));
    auto st = pool.stats();
    VORTEX_EXPECT_EQ(st.spawned, static_cast<uint64_t>(32));
    VORTEX_EXPECT_EQ(st.completed, static_cast<uint64_t>(32));

    // Deterministic steal: a blocker occupies its worker while the other
    // worker drains its own queue and steals from the blocked one's tail.
    // 500 ms (M6-b review F9): the one jitter-coupled assertion gets a
    // window a descheduled worker cannot plausibly miss.
    WorkStealingPool pool2(2);
    uint64_t block_ms = 500;
    auto b = pool2.spawn(blocker_entry, &block_ms);
    VORTEX_EXPECT(b.has_value());
    std::atomic<uint64_t> quick{0};
    FiberWork quick_work{&quick, 16};
    std::vector<uint64_t> quick_ids;
    for (int i = 0; i < 8; ++i) {
        auto s = pool2.spawn(counting_entry, &quick_work);
        VORTEX_EXPECT(s.has_value());
        if (s) quick_ids.push_back(*s);
    }
    for (uint64_t id : quick_ids) VORTEX_EXPECT(pool2.join(id).has_value());
    VORTEX_EXPECT_EQ(quick.load(), static_cast<uint64_t>(8));
    VORTEX_EXPECT(pool2.stats().stolen >= 1);
    VORTEX_EXPECT(pool2.join(*b).has_value());
}

// ---- DoD 9: cooperative yield rotates execution ------------------------------

namespace {
struct YieldArg {
    WorkStealingPool* pool;
    std::atomic<uint64_t>* counter;
    uint64_t rounds;
};

/// CEP:WHAT: Fiber entry: counted cooperative yields (rotation test).
/// CEP:WHY: Yield rotation must not starve any fiber.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — yield failures propagate.
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
support::Result<void> yielder_entry(void* user) {
    auto* a = static_cast<YieldArg*>(user);
    for (uint64_t i = 0; i < a->rounds; ++i) {
        a->counter->fetch_add(1, std::memory_order_release);
        auto y = a->pool->yield_current();
        if (!y) return std::unexpected(y.error());
    }
    return support::ok();
}
}  // namespace

/// CEP:WHAT: DoD 9: cooperative yield rotates execution without starvation.
/// CEP:WHY: Every fiber reaches its round count through the rotation.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(workstealing_yield_rotates_execution) {
    WorkStealingPool pool(1);
    std::atomic<uint64_t> c0{0}, c1{0}, c2{0};
    YieldArg a0{&pool, &c0, 50}, a1{&pool, &c1, 50}, a2{&pool, &c2, 50};
    auto f0 = pool.spawn(yielder_entry, &a0);
    auto f1 = pool.spawn(yielder_entry, &a1);
    auto f2 = pool.spawn(yielder_entry, &a2);
    VORTEX_EXPECT(f0.has_value() && f1.has_value() && f2.has_value());
    VORTEX_EXPECT(pool.join(*f0).has_value());
    VORTEX_EXPECT(pool.join(*f1).has_value());
    VORTEX_EXPECT(pool.join(*f2).has_value());
    // No fiber starved: all three reached 50 through the rotation.
    VORTEX_EXPECT_EQ(c0.load(), static_cast<uint64_t>(50));
    VORTEX_EXPECT_EQ(c1.load(), static_cast<uint64_t>(50));
    VORTEX_EXPECT_EQ(c2.load(), static_cast<uint64_t>(50));
    VORTEX_EXPECT(pool.stats().yields >= 147);
}

// ---- DoD 10: join propagates the entry result; shutdown drains ---------------

/// CEP:WHAT: DoD 10: join propagates the entry result; shutdown drains.
/// CEP:WHY: Entry failures surface through join by name (Rule 114).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(workstealing_join_collects_entry_result) {
    WorkStealingPool pool(2);
    auto bad = pool.spawn(
        [](void*) -> support::Result<void> {
            return support::fail(support::ErrorCode::InvalidArgument,
                                 "threading-test: boom");
        },
        nullptr);
    VORTEX_EXPECT(bad.has_value());
    if (!bad) return;
    auto j = pool.join(*bad);
    VORTEX_EXPECT(!j.has_value());
    if (!j) {
        VORTEX_EXPECT(j.error().message.find("boom") != std::string::npos);
        VORTEX_EXPECT(j.error().code == support::ErrorCode::InvalidArgument);
    }

    std::atomic<uint64_t> counter{0};
    FiberWork work{&counter, 8};
    std::vector<uint64_t> ids;
    for (int i = 0; i < 6; ++i) {
        auto s = pool.spawn(counting_entry, &work);
        VORTEX_EXPECT(s.has_value());
        if (s) ids.push_back(*s);
    }
    VORTEX_EXPECT(pool.shutdown().has_value());
    VORTEX_EXPECT(pool.shutdown_complete());
    VORTEX_EXPECT_EQ(counter.load(), static_cast<uint64_t>(6));
    // Spawn after shutdown rejects by name.
    auto after = pool.spawn(counting_entry, &work);
    VORTEX_EXPECT(!after.has_value());
    if (!after) {
        VORTEX_EXPECT(after.error().message.find("shutdown") !=
                      std::string::npos);
    }
    // Shutdown is idempotent.
    VORTEX_EXPECT(pool.shutdown().has_value());
    // The destructor completes the drain (scope exit below).
}

// ---- DoD (freeze integration): cooperative freeze over polling fibers -------

namespace {
struct FreezeArg {
    WorkStealingPool* pool;
    std::atomic<int>* stop;
    std::atomic<uint64_t>* done;
};

/// CEP:WHAT: Fiber entry: poll_hook loop (the cooperative freeze consumer).
/// CEP:WHY: The exact code an adopted interpreter's SAFEPOINT_POLL slow path runs.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — the hook absorbs the freeze window.
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
support::Result<void> freeze_poll_entry(void* user) {
    auto* a = static_cast<FreezeArg*>(user);
    // The pool's own poll hook: the exact code an adopted interpreter's
    // SAFEPOINT_POLL slow path runs. The loop only ends when the test
    // releases it — the fibers are GUARANTEED parked at a poll when the
    // freeze window is observed.
    while (!a->stop->load(std::memory_order_acquire)) {
        WorkStealingPool::poll_hook(a->pool);
        sched_yield();
    }
    a->done->fetch_add(1, std::memory_order_release);
    return support::ok();
}
}  // namespace

/// CEP:WHAT: DoD 11a: cooperative freeze holds polling fibers at the poll.
/// CEP:WHY: The poll hook is the exact code an adopted interpreter runs.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(freeze_over_polling_fibers_cooperative) {
    PageQuiet quiet;
    HandshakeManager hs;
    WorkStealingPool pool(2);
    // Both arguments required: a null page rejects by name...
    auto bad_bind = pool.bind_suspension(&hs, nullptr);
    VORTEX_EXPECT(!bad_bind.has_value());
    VORTEX_EXPECT(SafepointPollingPage::reserve().has_value());
    // ...then the REAL bind: every fiber spawned below registers a
    // handshake slot (M6-b review M6) and acknowledges a freeze from its
    // poll loop — the freeze over these fibers is REAL, not vacuous.
    SafepointPollingPage page;
    VORTEX_EXPECT(pool.bind_suspension(&hs, &page).has_value());
    // No fibers spawned yet: the freeze is instant but still arms the page.
    VORTEX_EXPECT(
        hs.freeze_all(SuspensionKind::Gc, std::chrono::seconds(1)).has_value());
    VORTEX_EXPECT(hs.thaw_all().has_value());

    std::atomic<int> stop{0};
    std::atomic<uint64_t> done{0};
    FreezeArg arg{&pool, &stop, &done};
    std::vector<uint64_t> ids;
    for (int i = 0; i < 4; ++i) {
        auto s = pool.spawn(freeze_poll_entry, &arg);
        VORTEX_EXPECT(s.has_value());
        if (s) ids.push_back(*s);
    }
    // Let the fibers reach their poll loops, then freeze: the fibers'
    // next poll_hook acknowledges (their spawn-time handshake slots) and
    // spins (and yields) inside the armed window.
    auto frozen = hs.freeze_all(SuspensionKind::Gc, std::chrono::seconds(2));
    VORTEX_EXPECT(frozen.has_value());
    VORTEX_EXPECT_EQ(done.load(), static_cast<uint64_t>(0));
    // Spawn during the freeze window rejects by name (M6-b review M5).
    auto frozen_spawn = pool.spawn(freeze_poll_entry, &arg);
    VORTEX_EXPECT(!frozen_spawn.has_value());
    if (!frozen_spawn) {
        VORTEX_EXPECT(frozen_spawn.error().message.find("freeze window") !=
                      std::string::npos);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    VORTEX_EXPECT_EQ(done.load(), static_cast<uint64_t>(0));  // held at the poll
    VORTEX_EXPECT(pool.stats().freeze_polls > 0);
    VORTEX_EXPECT(hs.thaw_all().has_value());
    stop.store(1, std::memory_order_release);
    for (uint64_t id : ids) VORTEX_EXPECT(pool.join(id).has_value());
    VORTEX_EXPECT_EQ(done.load(), static_cast<uint64_t>(4));
    VORTEX_EXPECT(pool.shutdown().has_value());
}

// ---- DoD 11: preemptive guard-page suspension, bounded latency ---------------

namespace {
constexpr int kDescendDepth = 200;

/// CEP:WHAT: Recursion with fat frames: the preemptive suspension's descent target.
/// CEP:WHY: The descent must cross the armed page within a bounded frame count.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — pure arithmetic.
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
__attribute__((noinline)) uint64_t descend(int depth, uint64_t acc) {
    volatile uint64_t pad[8];
    for (int i = 0; i < 8; ++i) pad[i] = acc + static_cast<uint64_t>(i);
    if (depth <= 0) {
        uint64_t sum = 0;
        for (int i = 0; i < 8; ++i) sum += pad[i];
        return sum;
    }
    return descend(depth - 1, acc + 1) + pad[0];
}

struct PreemptArg {
    std::atomic<int>* go;
    std::atomic<int>* done;
    std::atomic<uint64_t>* sink;
    std::atomic<uint64_t>* frame;  // runtime frame address of the fiber
};

/// CEP:WHAT: Fiber entry: wait-gate then a call-heavy loop with NO polls.
/// CEP:WHY: The guard-page trap must suspend it without any cooperative poll.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — the suspender owns the suspension.
/// CEP:ASSUMES: The module-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
support::Result<void> preempt_entry(void* user) {
    auto* a = static_cast<PreemptArg*>(user);
    a->frame->store(reinterpret_cast<uint64_t>(__builtin_frame_address(0)),
                    std::memory_order_release);
    // Shallow wait (stays within the first page of the stack).
    while (!a->go->load(std::memory_order_acquire)) sched_yield();
    // Call-heavy loop: every iteration descends ~5 pages below the top.
    while (!a->done->load(std::memory_order_acquire)) {
        a->sink->fetch_add(descend(kDescendDepth, 1), std::memory_order_relaxed);
        sched_yield();
    }
    return support::ok();
}
}  // namespace

/// CEP:WHAT: DoD 11: preemptive guard-page suspension within a bounded budget.
/// CEP:WHY: Rule 81 for code that never polls; resume re-executes the faulting access.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(guard_page_preemptive_suspension_bounded_latency) {
    PageQuiet quiet;
    TrapOff trap;
    auto en = GuardPageSuspender::enable_trap_suspension();
    VORTEX_EXPECT(en.has_value());
    if (!en) return;
    trap.armed = true;

    // Driver-only pool: no worker threads — the suspension fault fires on
    // THIS thread, whose trap disposition and altstack this test owns (no
    // sanitizer thread-start races can displace them mid-test).
    WorkStealingPool pool(0);
    std::atomic<int> go{0}, done{0};
    std::atomic<uint64_t> sink{0};
    std::atomic<uint64_t> frame{0};
    PreemptArg arg{&go, &done, &sink, &frame};
    WorkStealingPool::GreenThread gt;
    gt.entry = preempt_entry;
    gt.user = &arg;
    gt.stack_size = 512 * 1024;
    auto target = pool.spawn(gt);
    VORTEX_EXPECT(target.has_value());
    if (!target) return;
    const uint64_t fid = *target;
    const void* stack_base = gt.stack_base;
    VORTEX_EXPECT(stack_base != nullptr);

    // Arm the descent backstop: a never-started fiber's basis is its
    // makecontext-primed RSP — the page below it is the LAST usable stack
    // page, which the entry's first frames occupy.
    const bool armed_ok = pool.suspend_fiber(fid).has_value();
    VORTEX_EXPECT(armed_ok);
    if (!armed_ok) return;

    // Drive: the entry starts, exits the wait loop, and its first descent
    // crosses the armed page — the trap suspends the fiber WITHOUT any
    // poll on its side.
    const auto t0 = std::chrono::steady_clock::now();
    go.store(1, std::memory_order_release);
    auto first = pool.resume_fiber(fid);
    VORTEX_EXPECT(first.has_value());
    if (first) VORTEX_EXPECT(*first == WorkStealingPool::RunOutcome::Suspended);
    const auto latency = std::chrono::steady_clock::now() - t0;
    // Rule 81: the preemptive path is bounded — a descent of at most one
    // page below the entry start, asserted generously against jitter.
    VORTEX_EXPECT(latency < std::chrono::seconds(1));

    // The recorded entry frame lives inside the fiber's own usable stack
    // (M6-b review F8: the basis quantity is OBSERVED, not trusted).
    // stack_base is the LOW end (guard-adjacent); frames start at the high
    // end and descend toward it.
    const uint64_t fr = frame.load(std::memory_order_acquire);
    VORTEX_EXPECT(fr != 0);
    VORTEX_EXPECT(fr >= reinterpret_cast<uint64_t>(stack_base));
    VORTEX_EXPECT(fr < reinterpret_cast<uint64_t>(stack_base) + 512 * 1024);

    // Resume with the done flag set: the faulting access re-executes on a
    // readable page; the loop observes the flag and finishes.
    done.store(1, std::memory_order_release);
    auto resumed = pool.resume_fiber(fid);
    VORTEX_EXPECT(resumed.has_value());
    if (resumed) VORTEX_EXPECT(*resumed == WorkStealingPool::RunOutcome::Done);
    auto st = pool.fiber_state(fid);
    VORTEX_EXPECT(st.has_value() && *st == 3);  // Done
    VORTEX_EXPECT(sink.load() > 0);
}

// ---- DoD 12: foreign faults chain to the previous handler --------------------

/// CEP:WHAT: DoD 12: foreign faults chain to the previous handler (fork-isolated).
/// CEP:WHY: Sanitizer reports and genuine crashes must never be eaten.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(guard_page_foreign_fault_chains_to_previous_handler) {
    PageQuiet quiet;
    TrapOff trap;
    auto en = GuardPageSuspender::enable_trap_suspension();
    VORTEX_EXPECT(en.has_value());
    if (!en) return;
    trap.armed = true;

    const pid_t pid = fork();
    VORTEX_EXPECT(pid >= 0);
    if (pid < 0) return;
    if (pid == 0) {
        // Child: silence the report noise, then take a genuine wild fault
        // (a non-null, mis-mapped address a sanitizer cannot statically
        // flag). The suspender must NOT eat this: it chains to the previous
        // disposition (sanitizer report / default signal).
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 2);
        }
        volatile char* wild = reinterpret_cast<volatile char*>(
            static_cast<uintptr_t>(0x7));
        *wild = 1;
        _exit(0);  // NOT reached when the chain works
    }
    int status = 0;
    VORTEX_EXPECT(waitpid(pid, &status, 0) == pid);
    const bool died = (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) ||
                      (WIFEXITED(status) && WEXITSTATUS(status) != 0);
    VORTEX_EXPECT(died);
    VORTEX_EXPECT(GuardPageSuspender::trap_suspension_enabled());
}

// ---- DoD 13: suspender region rejects -----------------------------------------

/// CEP:WHAT: DoD 13: suspender region rejects (null/zero/overlap/double-disarm).
/// CEP:WHY: Every failure is a named reject (Rule 9).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(suspender_arm_rejects_bad_regions) {
    const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* mem = mmap(nullptr, 2 * ps, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    VORTEX_EXPECT(mem != MAP_FAILED);
    if (mem == MAP_FAILED) return;
    auto* base = static_cast<unsigned char*>(mem);

    GuardPageSuspender::Record r1, r2, r3;
    VORTEX_EXPECT(!GuardPageSuspender::arm(nullptr, ps, &r1).has_value());
    VORTEX_EXPECT(!GuardPageSuspender::arm(base, 0, &r1).has_value());
    VORTEX_EXPECT(!GuardPageSuspender::arm(base, ps, nullptr).has_value());

    VORTEX_EXPECT(GuardPageSuspender::arm(base, ps, &r1).has_value());
    VORTEX_EXPECT(GuardPageSuspender::armed_for(base));
    VORTEX_EXPECT(GuardPageSuspender::armed_for(base + ps - 1));
    // Overlaps reject (same base, and a sub-page offset that aligns out to
    // the same page).
    VORTEX_EXPECT(!GuardPageSuspender::arm(base, ps, &r2).has_value());
    VORTEX_EXPECT(!GuardPageSuspender::arm(base + ps / 2, ps, &r3).has_value());
    // A misaligned base aligns outward onto an armed page (overlap reject).
    VORTEX_EXPECT(!GuardPageSuspender::arm(base + 7, ps, &r3).has_value());
    // The second page is free.
    VORTEX_EXPECT(GuardPageSuspender::arm(base + ps, ps, &r2).has_value());
    VORTEX_EXPECT(GuardPageSuspender::armed_for(base + ps + 1));

    // Disarm needs the armed BASE; double disarm rejects.
    VORTEX_EXPECT(GuardPageSuspender::disarm(base).has_value());
    VORTEX_EXPECT(!GuardPageSuspender::armed_for(base));
    VORTEX_EXPECT(!GuardPageSuspender::disarm(base).has_value());
    VORTEX_EXPECT(GuardPageSuspender::disarm(base + ps).has_value());
    munmap(mem, 2 * ps);

    // Registry capacity is a REAL bound (M6-b review): 1024 single-page
    // regions arm; the next rejects by name; disarming restores capacity.
    const size_t cap = GuardPageSuspender::kMaxRegions;
    void* big = mmap(nullptr, (cap + 1) * ps, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    VORTEX_EXPECT(big != MAP_FAILED);
    if (big != MAP_FAILED) {
        auto* b = static_cast<unsigned char*>(big);
        auto recs =
            std::make_unique<GuardPageSuspender::Record[]>(cap);
        bool all_ok = true;
        for (size_t i = 0; i < cap; ++i) {
            if (!GuardPageSuspender::arm(b + i * ps, ps, &recs[i])
                     .has_value()) {
                all_ok = false;
                break;
            }
        }
        VORTEX_EXPECT(all_ok);
        auto overflow = GuardPageSuspender::arm(b + cap * ps, ps, &r3);
        VORTEX_EXPECT(!overflow.has_value());
        if (!overflow) {
            VORTEX_EXPECT(overflow.error().message.find("registry full") !=
                          std::string::npos);
        }
        for (size_t i = 0; i < cap; ++i) {
            (void)GuardPageSuspender::disarm(b + i * ps);
        }
        munmap(big, (cap + 1) * ps);
    }
}

// ---- DoD 14: handshake bounded-resource rejects --------------------------------

/// CEP:WHAT: DoD 14: handshake bounded-resource rejects + id lifecycle.
/// CEP:WHY: Unknown ids and the permanent anonymous slot are documented contracts.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(handshake_unknown_ids_named_rejects) {
    HandshakeManager hs;
    // Out-of-range ids reject everywhere.
    VORTEX_EXPECT(!hs.request(SuspensionKind::Gc, 1'000'000).has_value());
    VORTEX_EXPECT(!hs.acknowledge(1'000'000).has_value());
    VORTEX_EXPECT(!hs.wait_acknowledged(1'000'000, std::chrono::milliseconds(1))
                       .has_value());
    VORTEX_EXPECT(!hs.unregister_thread(1'000'000).has_value());
    // The anonymous slot is permanent.
    VORTEX_EXPECT(!hs.unregister_thread(0).has_value());

    // Register/unregister lifecycle: a released id stops accepting work.
    auto r = hs.register_thread();
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    VORTEX_EXPECT(hs.unregister_thread(*r).has_value());
    VORTEX_EXPECT(!hs.request(SuspensionKind::Gc, *r).has_value());
    VORTEX_EXPECT(!hs.acknowledge(*r).has_value());
    VORTEX_EXPECT(!hs.unregister_thread(*r).has_value());
    VORTEX_EXPECT(!hs.pending(*r));
    // The id is reusable after unregistration.
    auto r2 = hs.register_thread();
    VORTEX_EXPECT(r2.has_value() && *r2 == *r);
    if (r2) (void)hs.unregister_thread(*r2);

    // Table capacity is a REAL bound (M6-b review): kMaxThreads - 1 ids
    // register (slot 0 is the permanent anonymous slot); the next rejects
    // by name; unregistering restores capacity.
    {
        std::vector<uint64_t> ids;
        ids.reserve(HandshakeManager::kMaxThreads);
        for (size_t i = 1; i < HandshakeManager::kMaxThreads; ++i) {
            auto r3 = hs.register_thread();
            if (!r3) break;
            ids.push_back(*r3);
        }
        VORTEX_EXPECT_EQ(ids.size(), HandshakeManager::kMaxThreads - 1);
        VORTEX_EXPECT(!hs.register_thread().has_value());
        for (uint64_t id : ids) (void)hs.unregister_thread(id);
        auto r4 = hs.register_thread();
        VORTEX_EXPECT(r4.has_value());
        if (r4) (void)hs.unregister_thread(*r4);
    }
}

// ---- DoD 11b (M6-b review M10): the re-based patch basis catches a STARTED
//      DEEP fiber that never polls -------------------------------------------

namespace {
struct DeepArg {
    WorkStealingPool* pool;
    std::atomic<int>* go;
    std::atomic<int>* done;
    std::atomic<uint64_t>* sink;
};

/// CEP:WHAT: Fiber entry: parks at a gate yield, then a deep no-poll loop.
/// CEP:WHY: Every park re-bases the suspension basis (run_on_current post-
///           save); the deep loop after go descends BELOW that basis — the
///           shape the entry-frame basis could never catch.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — the suspender owns the suspension.
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
support::Result<void> deep_entry(void* user) {
    auto* a = static_cast<DeepArg*>(user);
    auto y = a->pool->yield_current();  // park: re-bases the basis
    if (!y) return std::unexpected(y.error());
    while (!a->go->load(std::memory_order_acquire)) {
        auto y2 = a->pool->yield_current();  // gate parks keep re-basing
        if (!y2) return std::unexpected(y2.error());
    }
    // Deep loop, NO polls and NO yields: descends ~5 pages below the last
    // park and stays there.
    while (!a->done->load(std::memory_order_acquire)) {
        a->sink->fetch_add(descend(kDescendDepth, 1),
                           std::memory_order_relaxed);
    }
    return support::ok();
}
}  // namespace

/// CEP:WHAT: DoD 11b: the re-based basis suspends a STARTED deep fiber.
/// CEP:WHY: The production preemptive shape — a fiber already deeper than
///           its entry frame when the request lands (M6-b review M10).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(guard_page_rebased_basis_catches_started_deep_fiber) {
    PageQuiet quiet;
    TrapOff trap;
    auto en = GuardPageSuspender::enable_trap_suspension();
    VORTEX_EXPECT(en.has_value());
    if (!en) return;
    trap.armed = true;

    WorkStealingPool pool(0);  // driver-only: deterministic driving
    std::atomic<int> go{0}, done{0};
    std::atomic<uint64_t> sink{0};
    DeepArg arg{&pool, &go, &done, &sink};
    auto target = pool.spawn(deep_entry, &arg, 512 * 1024);
    VORTEX_EXPECT(target.has_value());
    if (!target) return;
    const uint64_t fid = *target;

    // Park the fiber at least once: the basis is now its LIVE yield depth.
    auto r = pool.resume_fiber(fid);
    VORTEX_EXPECT(r.has_value());
    if (r) VORTEX_EXPECT(*r == WorkStealingPool::RunOutcome::Yielded);

    // Arm the patch (basis = the last park), then let it descend.
    const bool armed_ok = pool.suspend_fiber(fid).has_value();
    VORTEX_EXPECT(armed_ok);
    if (!armed_ok) return;
    go.store(1, std::memory_order_release);
    const auto t0 = std::chrono::steady_clock::now();
    auto next = pool.resume_fiber(fid);
    VORTEX_EXPECT(next.has_value());
    if (next) VORTEX_EXPECT(*next == WorkStealingPool::RunOutcome::Suspended);
    VORTEX_EXPECT(std::chrono::steady_clock::now() - t0 <
                  std::chrono::seconds(1));

    // Resume: the faulting descent re-executes on the readable page, the
    // loop sees done, the fiber completes.
    done.store(1, std::memory_order_release);
    auto fin = pool.resume_fiber(fid);
    VORTEX_EXPECT(fin.has_value());
    if (fin) VORTEX_EXPECT(*fin == WorkStealingPool::RunOutcome::Done);
    VORTEX_EXPECT(sink.load() > 0);
}

// ---- DoD 11c (M6-b review B3): a lifetime-guard fault FAILS the fiber ------

namespace {
/// CEP:WHAT: Unbounded recursion: the lifetime low-guard overflow target.
/// CEP:WHY: The guard must FAIL the fiber by name, never suspend it.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — the guard fault is the failure surface.
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
__attribute__((noinline)) uint64_t overflow_rec(int n) {
    volatile uint64_t pad[64];
    for (int i = 0; i < 64; ++i) pad[i] = static_cast<uint64_t>(n + i);
    if (pad[0] == 0xdead'beefull) return n;  // never true; defeats opts
    return overflow_rec(n + 1) + pad[1];
}

/// CEP:WHAT: Fiber entry: recurses until the lifetime guard faults.
/// CEP:WHY: Exercises the B3 contract end to end through the scheduler.
/// CEP:STATUS: complete
/// CEP:FAILURE: none — the named failure is the expected outcome.
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
support::Result<void> overflow_entry(void* user) {
    (void)user;
    (void)overflow_rec(0);  // never returns
    return support::ok();
}
}  // namespace

/// CEP:WHAT: DoD 11c: a stack overflow into the lifetime low guard fails
///           the fiber by name (never suspended, guard never disarmed).
/// CEP:WHY: Disarming the lifetime guard would turn a bounded overflow into
///           silent corruption below the mapping (M6-b review B3).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(fiber_stack_overflow_fails_by_name) {
    PageQuiet quiet;
    TrapOff trap;
    auto en = GuardPageSuspender::enable_trap_suspension();
    VORTEX_EXPECT(en.has_value());
    if (!en) return;
    trap.armed = true;

    WorkStealingPool pool(0);
    auto target = pool.spawn(overflow_entry, nullptr, 256 * 1024);
    VORTEX_EXPECT(target.has_value());
    if (!target) return;
    const uint64_t fid = *target;

    auto first = pool.resume_fiber(fid);
    VORTEX_EXPECT(first.has_value());
    if (first) VORTEX_EXPECT(*first == WorkStealingPool::RunOutcome::Done);
    auto st = pool.fiber_state(fid);
    VORTEX_EXPECT(st.has_value() && *st == 3);  // Done
    // The named failure propagates through join.
    auto j = pool.join(fid);
    VORTEX_EXPECT(!j.has_value());
    if (!j) {
        VORTEX_EXPECT(j.error().message.find("stack overflow") !=
                      std::string::npos);
    }
}

// ---- DoD 4b (M6-b review gap): the J1 LOCAL fallback word is exercisable ----

/// CEP:WHAT: DoD 4b: the per-executable fallback word suspends J1 too.
/// CEP:WHY: The global-word path is pinned by DoD 4; the fallback binding
///           (page never reserved) must behave identically.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness
/// CEP:ASSUMES: The file-level block covers the shared contracts.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: this test is the evidence (DoD, docs/infrastructure/04 section 10).
VORTEX_TEST(j1_local_fallback_word_suspends) {
    PageQuiet quiet;
    StencilTable corpus;
    j1::build_default_corpus(corpus);
    J1Rig rig(&corpus);
    const bool built = rig.build();
    VORTEX_EXPECT(built);
    if (!built) return;
    // Point the context at the per-executable fallback word explicitly (the
    // global page may already be reserved by an earlier test in this
    // process — the fallback contract is exercised by the binding itself).
    rig.bindings.context.safepoint_word = &rig.bindings.safepoint_word;
    rig.bindings.safepoint_word = 0;
    auto ok_run = rig.run();
    VORTEX_EXPECT(ok_run.has_value());
    if (ok_run) VORTEX_EXPECT_EQ(ok_run->as_smi(), 0);

    // Armed LOCAL word: the stencil raises the named safepoint request.
    rig.bindings.safepoint_word = 0x8000'0001u;
    auto armed_run = rig.run();
    VORTEX_EXPECT(!armed_run.has_value());
    if (!armed_run) {
        VORTEX_EXPECT(armed_run.error().message.find("safepoint request") !=
                      std::string::npos);
    }
    rig.bindings.safepoint_word = 0;
}

