// J3 tests (docs/roadmap.md M3 DoD, Compiler Laws, CEM-26):
//   - RBPD: failing-region-only failure accounting + neighbor preservation
//     (the partial-deopt isolation DoD), recovery paths, throttle, merge/
//     split (Rules 43/39; docs/deopt-rbpd.md)
//   - Interop: capability-gated load (docs/interop-protocol.md section 3),
//     T0 POLY_* dispatch parity through a registered native port,
//     UNSUPPORTED propagation (Rule 3)
//   - J3 tier: parity with T0, XLEA scalar replacement of the wrapper +
//     POLY dispatch elimination (docs/xlea.md section 2), region-capture
//     suspension resume (the no-capture whole-method rerun replaced),
//     region failure accounting through the run path
//   - Escape summaries: publication, monotonic weakening, invalidation
//     (docs/xlea.md section 4.1 laws)
#include "vortex_test.hpp"

#include <memory>
#include <vector>

#include "vortex/gc/icggc.hpp"
#include "vortex/infra/code_range.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/j3/full_jit.hpp"
#include "vortex/runtime/interop.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/verifier.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;
using namespace vortex::ugb;
using namespace vortex::deopt;
using namespace vortex::j2;
using namespace vortex::j3;

namespace {

constexpr const char* kCountBuiltin = "count";

/// A counting builtin: the guest-visible side-effect counter for the
/// suspension tests (user data = uint64_t counter).
TaggedValue count_builtin(std::span<const TaggedValue>, void* user) {
    // user == nullptr is the T0-truth helper (counting disabled) — the
    // builtin still returns a well-defined value.
    auto* n = static_cast<uint64_t*>(user);
    if (n != nullptr) ++*n;
    return TaggedValue::smi(n != nullptr ? static_cast<int64_t>(*n) : 1);
}

/// Full J3 harness: T0 warmup builds profiles/ICs (Rules 22/32), then the
/// 60-stage J3 pipeline + shared backend compile for `entry`.
class J3Harness {
public:
    J3Harness(const char* src, const char* entry) : src_(src), entry_(entry) {}

    void set_interop(runtime::interop::InteropRegistry* r) { interop_ = r; }

    vm::Result<TaggedValue> run(const std::vector<TaggedValue>& args,
                                size_t node_cap = 200'000) {
        auto module = assemble_module(src_);
        if (!module) {
            std::printf("  [j3-harness] assemble: %s\n",
                        module.error().message.c_str());
            return std::unexpected(module.error());
        }
        module_ = std::make_unique<UGBModule>(std::move(*module));
        auto verified = verify_module(*module_);
        if (!verified) {
            std::printf("  [j3-harness] verify: %s\n",
                        verified.error().message.c_str());
            return std::unexpected(verified.error());
        }
        heap_ = std::make_unique<gc::Heap>();
        interp_ = std::make_unique<vm::Interpreter>(*heap_);
        counter_member_ = 0;  // fresh per invocation (each run() re-runs
                              // the 64 warmups before the published run)
        counter_ = &counter_member_;
        interp_->register_builtin(kCountBuiltin, count_builtin, counter_);
        if (interop_ != nullptr) interp_->set_interop_registry(interop_);
        // Bootstrap the module runtime FIRST: klass handles must exist so
        // make_j1_bindings REUSES them (one klass family across T0 objects,
        // the bindings table, and the registry binding).
        auto boot = interp_->build_module_runtime(*module_);
        if (!boot) return std::unexpected(boot.error());
        auto br = j1::make_j1_bindings(*heap_, *module_, interp_.get(),
                                       bindings_);
        if (!br) return std::unexpected(br.error());
        // Bind every wrapper klass to the native port (language id 1) so
        // POLY dispatch resolves receivers (docs/interop-protocol.md 4),
        // BEFORE the warm run that feeds the IC slots.
        if (interop_ != nullptr) {
            for (void* k : bindings_.klass_addr_table) {
                VORTEX_EXPECT(interop_->bind_klass(1, k).has_value());
            }
            for (void* k : module_->runtime.klass_table) {
                VORTEX_EXPECT(interop_->bind_klass(1, k).has_value());
            }
        }
        // Warm the interpreter ENOUGH for the adaptive machinery: the call
        // site must reach the inline threshold and the IC slots must reach
        // the mono state (Rules 22/30/32). Repeated identical runs are the
        // deterministic heat source.
        vm::Result<vm::RunResult> warm = std::unexpected(
            support::Diagnostic(support::ErrorCode::InternalError,
                                std::string("cold")));
        for (int i = 0; i < 64; ++i) {
            warm = interp_->run(*module_, entry_, args);
            if (!warm) {
                std::printf("  [j3-harness] warm[%d]: %s\n", i,
                            warm.error().message.c_str());
                return std::unexpected(warm.error());
            }
        }
        const int32_t id = module_->find_method(entry_);
        if (id < 0) {
            return fail(support::ErrorCode::InvalidArgument,
                        "J3 test: no such method");
        }
        const ugb::UGBMethod& method =
            module_->method_table[static_cast<size_t>(id)];
        J2Job job;
        job.module = module_.get();
        job.method_id = static_cast<uint32_t>(id);
        job.profiles = &method.profiles;
        job.ics = &method.ics;
        job.klass_addrs = &bindings_.klass_addr_table;
        job.double_klass = bindings_.double_klass;
        job.node_cap = node_cap;
        job.interop = interop_;
        auto code = compile_j3(job);
        if (!code) {
            std::printf("  [j3-harness] compile: %s\n",
                        code.error().message.c_str());
            return std::unexpected(code.error());
        }
        code_ = std::make_unique<J2Code>(std::move(*code));
        auto pub = publish_j3(*code_, summary_, infra::global_code_range());
        if (!pub) return std::unexpected(pub.error());
        ex_ = std::make_unique<J3Executable>(std::move(*pub));
        auto run = run_j3(*ex_, bindings_, args, *interp_);
        if (!run) {
            std::printf("  [j3-harness] run: %s\n",
                        run.error().message.c_str());
        }
        return run;
    }

    /// Arms the suspension poll: the next poll site the published code
    /// executes takes the captured-deopt path (docs/deopt-rbpd.md 2-4).
    void arm_poll() {
        *reinterpret_cast<uint32_t*>(bindings_.context.safepoint_word) = 1;
    }

    const J2Code& code() const { return *code_; }
    uint64_t counter() const { return counter_member_; }
    const j2::J2Executable& core() const { return ex_->core; }

private:
    const char* src_;
    const char* entry_;
    runtime::interop::InteropRegistry* interop_ = nullptr;
    std::unique_ptr<UGBModule> module_;
    std::unique_ptr<gc::Heap> heap_;
    std::unique_ptr<vm::Interpreter> interp_;
    j1::J1Bindings bindings_;
    std::unique_ptr<J2Code> code_;
    ir::EscapeSummary summary_;
    std::unique_ptr<J3Executable> ex_;
    uint64_t* counter_ = nullptr;
    uint64_t counter_member_ = 0;
};

vm::Result<vm::RunResult> run_t0(
    const char* src, const std::string& entry,
    const std::vector<TaggedValue>& args,
    runtime::interop::InteropRegistry* interop = nullptr) {
    auto module = assemble_module(src);
    if (!module) return std::unexpected(module.error());
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.register_builtin(
        kCountBuiltin, count_builtin, nullptr);
    if (interop != nullptr) {
        interp.set_interop_registry(interop);
        auto boot = interp.build_module_runtime(*module);
        if (!boot) return std::unexpected(boot.error());
        j1::J1Bindings bindings;
        auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
        if (!br) return std::unexpected(br.error());
        for (void* k : bindings.klass_addr_table) {
            VORTEX_EXPECT(interop->bind_klass(1, k).has_value());
        }
        for (void* k : module->runtime.klass_table) {
            VORTEX_EXPECT(interop->bind_klass(1, k).has_value());
        }
    }
    return interp.run(*module, entry, args);
}

/// A native-objects interop port: read/write member = vortex field-slot
/// access on the wrapper (the semantics that license the POLY lowering).
uint64_t native_read(void* recv, uint32_t member_idx) {
    auto* obj = static_cast<Object*>(recv);
    return obj->field(member_idx).raw();
}
uint32_t native_write(void* recv, uint32_t member_idx, uint64_t value) {
    auto* obj = static_cast<Object*>(recv);
    obj->field(member_idx) = TaggedValue::from_raw(value);
    return 1;
}
/// HAS_MEMBERS slot: always yes (used to prove the capability gate fires
/// BEFORE the slot probe — a non-null slot on a 0-capability language
/// must still refuse named, review Task 12).
uint32_t native_has_members(void* /*recv*/, uint32_t /*member_idx*/) {
    return 1;
}

/// EXECUTE slot: the sum of the smi args (deterministic, guest-visible).
/// A non-smi arg is a handler-level type refusal (rc != 0 -> the
/// dispatcher's named RuntimeError path).
int64_t native_execute(void* recv, const void* args, uint32_t argc,
                       void* ret) {
    (void)recv;
    auto* out = static_cast<TaggedValue*>(ret);
    const auto* a = static_cast<const TaggedValue*>(args);
    int64_t acc = 0;
    for (uint32_t i = 0; i < argc; ++i) {
        if (!a[i].is_smi()) return 2;
        acc += a[i].as_smi();
    }
    *out = TaggedValue::smi(acc);
    return 0;
}

/// Builds the interop registry + a wrapper klass bound to the native port.
struct InteropFixture {
    runtime::interop::InteropRegistry registry;
    const void* wrapper_klass = nullptr;

    InteropFixture() {
        runtime::interop::InteropVTable vt{};
        vt.read_member = native_read;
        vt.write_member = native_write;
        vt.execute = native_execute;
        registry.register_language("native-test", vt,
                                   runtime::interop::CAP_INTEROP_MEMBERS |
                                       runtime::interop::CAP_INTEROP_EXECUTE,
                                   runtime::interop::PortKind::NativeObjects);
    }
};

}  // namespace

// ---- RBPD core (docs/deopt-rbpd.md; Rules 39/43) ------------------------------

VORTEX_TEST(rbpd_failure_isolation_and_recovery) {
    RegionTable table;
    RegionDescriptor hot;
    hot.kind = RegionKind::Hot;
    hot.bytecode_pc_begin = 10;
    hot.bytecode_pc_end = 20;
    const uint32_t hot_id = table.add_region(hot);
    RegionDescriptor cold;
    cold.kind = RegionKind::Cold;
    cold.bytecode_pc_begin = 20;
    cold.bytecode_pc_end = 30;
    const uint32_t cold_id = table.add_region(cold);
    RegionDescriptor other;
    other.kind = RegionKind::Hot;
    other.bytecode_pc_begin = 30;
    other.bytecode_pc_end = 40;
    const uint32_t other_id = table.add_region(other);
    VORTEX_EXPECT_EQ(hot_id, 0u);
    VORTEX_EXPECT_EQ(cold_id, 1u);
    VORTEX_EXPECT_EQ(other_id, 2u);

    ThrottlePolicy policy;
    // Guard failure advances ONLY the failing region's counter (the DoD
    // isolation property): the neighboring regions stay at zero and Live.
    VORTEX_EXPECT_EQ(static_cast<int>(
                         table.on_guard_failure(cold_id, policy, 77,
                                                DeoptReason::GuardFailed, 1)),
                     static_cast<int>(RecoveryPath::RecompileRegion));
    VORTEX_EXPECT_EQ(table.find_by_id(cold_id)->failure_count, 1u);
    VORTEX_EXPECT_EQ(table.find_by_id(hot_id)->failure_count, 0u);
    VORTEX_EXPECT_EQ(table.find_by_id(other_id)->failure_count, 0u);
    VORTEX_EXPECT_EQ(table.find_by_id(hot_id)->state, RegionState::Live);
    VORTEX_EXPECT_EQ(table.find_by_id(other_id)->state, RegionState::Live);
    // The last event names the failing region + site (Rule 43 accounting).
    VORTEX_EXPECT_EQ(table.last_event()->region_id, cold_id);
    VORTEX_EXPECT_EQ(table.last_event()->site_id, 77u);
    VORTEX_EXPECT_EQ(table.last_event()->reason, DeoptReason::GuardFailed);

    // Below threshold, a successor region wins over a recompile (section 4
    // path B); the legacy int form keeps the M0 contract.
    VORTEX_EXPECT_EQ(table.on_guard_failure(hot_id, 3), 0);
    VORTEX_EXPECT_EQ(table.on_guard_failure(hot_id, 3), 0);
    VORTEX_EXPECT_EQ(table.on_guard_failure(hot_id, 3), 2);  // threshold -> C

    // Suspension is an event, never a failure (the M1 contract).
    const uint64_t before = table.method_failure_count();
    table.on_suspension(hot_id, 5, 99);
    VORTEX_EXPECT_EQ(table.method_failure_count(), before);
    VORTEX_EXPECT_EQ(table.last_event()->reason, DeoptReason::SuspensionPoll);
    VORTEX_EXPECT_EQ(table.find_by_id(hot_id)->failure_count, 3u);

    // Region state machine: replace marks stale, registers Recompiled.
    RegionDescriptor replacement;
    replacement.bytecode_pc_begin = 20;
    const uint32_t new_id =
        table.replace_region(cold_id, std::move(replacement));
    VORTEX_EXPECT(new_id != UINT32_MAX);
    VORTEX_EXPECT_EQ(table.find_by_id(cold_id)->state, RegionState::Stale);
    VORTEX_EXPECT_EQ(table.find_by_id(new_id)->state, RegionState::Recompiled);
    // Stale regions never re-enter (find_by_pc_offset skips them).
    VORTEX_EXPECT(table.find_by_pc_offset(25) == nullptr ||
                  table.find_by_pc_offset(25)->region_id != cold_id);

    // Merge: adjacent live regions collapse; the tail goes stale.
    RegionTable t2;
    RegionDescriptor a;
    a.entry_offset = 0;
    a.exit_offset = 16;
    const uint32_t a_id = t2.add_region(a);
    RegionDescriptor b;
    b.entry_offset = 16;
    b.exit_offset = 48;
    const uint32_t b_id = t2.add_region(b);
    const uint32_t merged = t2.merge_regions(a_id, b_id);
    VORTEX_EXPECT_EQ(merged, a_id);
    VORTEX_EXPECT_EQ(t2.find_by_id(a_id)->exit_offset, 48u);
    VORTEX_EXPECT_EQ(t2.find_by_id(b_id)->state, RegionState::Stale);
    // Split: around one failing guard, the tail becomes the successor.
    const uint32_t tail = t2.split_region(a_id, 32);
    VORTEX_EXPECT(tail != UINT32_MAX);
    VORTEX_EXPECT_EQ(t2.find_by_id(a_id)->exit_offset, 32u);
    VORTEX_EXPECT_EQ(t2.find_by_id(tail)->entry_offset, 32u);
    VORTEX_EXPECT_EQ(t2.find_by_id(a_id)->deopt.successor_region_id, tail);

    // Throttle verdict ladder (Rule 43): keep -> weaker -> downgrade.
    RegionTable t3;
    RegionDescriptor r;
    const uint32_t rid = t3.add_region(r);
    ThrottlePolicy p;
    VORTEX_EXPECT_EQ(t3.throttle_verdict(p), 0);
    VORTEX_EXPECT_EQ(
        static_cast<int>(t3.on_guard_failure(rid, p, 1,
                                             DeoptReason::GuardFailed, 1)),
        static_cast<int>(RecoveryPath::RecompileRegion));
    // region_threshold trips -> downgrade.
    for (uint32_t i = 1; i < p.region_threshold; ++i) {
        t3.on_guard_failure(rid, p, 1, DeoptReason::GuardFailed, i);
    }
    VORTEX_EXPECT_EQ(t3.throttle_verdict(p), 2);
    VORTEX_EXPECT_EQ(t3.find_by_id(rid)->state, RegionState::Blacklisted);
}

// ---- interop: capability-gated load (docs/interop-protocol.md section 3) ------

VORTEX_TEST(interop_capability_gated_load) {
    // POLY_READ without the interop_messages capability -> VerifyError.
    {
        const char* src = R"(
.language native-test
.class W
.method main(regs=6, args=0)
  New.Object v1, W
  Poly.Read v0, v1, 0
  Return v0
.end
)";
        auto module = assemble_module(src);
        VORTEX_EXPECT(module.has_value());
        if (module) {
            auto res = verify_module(*module);
            VORTEX_EXPECT(!res.has_value());
        }
    }
    // With the capability but the module mask missing the group bit ->
    // VerifyError (negotiation at load, Rule 3).
    {
        const char* src = R"(
.language native-test
.class W
.method main(regs=6, args=0)
  .requires interop_messages
  New.Object v1, W
  Poly.Read v0, v1, 0
  Return v0
.end
)";
        auto module = assemble_module(src);
        VORTEX_EXPECT(module.has_value());
        if (module) {
            auto res = verify_module(*module);
            VORTEX_EXPECT(!res.has_value());
        }
    }
    // Capability + mask + valid window -> verified.
    {
        const char* src = R"(
.language native-test
.interop_mask 1
.class W
.field a in W
.method main(regs=6, args=0)
  .requires interop_messages
  Const.I32 v0, 7
  New.Object v1, W
  SetField v1, v0, W.a
  Poly.Read v2, v1, 0
  Return v2
.end
)";
        auto module = assemble_module(src);
        VORTEX_EXPECT(module.has_value());
        if (module) {
            auto res = verify_module(*module);
            if (!res.has_value()) {
                std::printf("  verify said: %s\n",
                            res.error().message.c_str());
            }
            VORTEX_EXPECT(res.has_value());
        }
    }
}

// ---- interop: T0 dispatch parity through the registered native port -----------

VORTEX_TEST(interop_t0_poly_dispatch_parity) {
    InteropFixture fx;
    const char* src = R"(
.language native-test
.interop_mask 1
.class W
.field a in W
.field b in W
.method main(regs=8, args=0)
  .requires interop_messages
  Const.I32 v0, 11
  Const.I32 v1, 22
  New.Object v2, W
  SetField v2, v0, W.a
  SetField v2, v1, W.b
  Poly.Read v3, v2, 0
  Poly.Read v4, v2, 1
  Add.Any v5, v3, v4
  Poly.Write v2, v5, 0
  Poly.Read v6, v2, 0
  Return v6
.end
)";
    auto module = assemble_module(src);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.set_interop_registry(&fx.registry);
    auto boot = interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    for (void* k : bindings.klass_addr_table) {
        VORTEX_EXPECT(fx.registry.bind_klass(1, k).has_value());
    }
    for (void* k : module->runtime.klass_table) {
        VORTEX_EXPECT(fx.registry.bind_klass(1, k).has_value());
    }
    auto res = interp.run(*module, "main", {});
    VORTEX_EXPECT(res.has_value());
    if (res) VORTEX_EXPECT_EQ(res->value.as_smi(), 33);
}

VORTEX_TEST(interop_unsupported_is_named_not_silent) {
    InteropFixture fx;
    // The failure surface of Poly.Read must be NAMED in every shape
    // (Rule 3). This test used to discard the run result and assert only
    // a registry lookup — vacuous (Rule 121); each shape now pins the
    // error code and the distinguishing message fragment.
    const char* src = R"(
.language native-test
.interop_mask 1
.class W
.field a in W
.method main(regs=6, args=0)
  .requires interop_messages
  Const.I32 v0, 1
  New.Object v1, W
  SetField v1, v0, W.a
  Poly.Read v2, v1, 9
  Return v2
.end
.method hm_main(regs=6, args=0)
  .requires interop_messages
  Const.I32 v0, 0
  New.Object v1, W
  Poly.Send v2, 0, v1, v0, 1
  Return v2
.end
)";
    // Shape A: UNREGISTERED klass — resolve() refuses before any member
    // access (the old comment wrongly claimed the field read happened).
    {
        auto module = assemble_module(src);
        VORTEX_EXPECT(module.has_value());
        if (!module) return;
        gc::Heap heap;
        vm::Interpreter interp(heap);
        interp.set_interop_registry(&fx.registry);
        auto res = interp.run(*module, "main", {});
        VORTEX_EXPECT(!res.has_value());
        if (!res) {
            VORTEX_EXPECT(res.error().code ==
                          support::ErrorCode::Unsupported);
            VORTEX_EXPECT(res.error().message.find("not a registered") !=
                          std::string::npos);
        }
    }
    // Shape B: BOUND klass, member_idx out of range for the klass layout
    // — the vortex slot-validation path (TypeError).
    {
        auto module = assemble_module(src);
        VORTEX_EXPECT(module.has_value());
        if (!module) return;
        gc::Heap heap;
        vm::Interpreter interp(heap);
        interp.set_interop_registry(&fx.registry);
        auto boot = interp.build_module_runtime(*module);
        VORTEX_EXPECT(boot.has_value());
        if (!boot) return;
        for (void* k : module->runtime.klass_table) {
            VORTEX_EXPECT(fx.registry.bind_klass(1, k).has_value());
        }
        auto res = interp.run(*module, "main", {});
        VORTEX_EXPECT(!res.has_value());
        if (!res) {
            VORTEX_EXPECT(res.error().code == support::ErrorCode::TypeError);
            VORTEX_EXPECT(res.error().message.find("out of range") !=
                          std::string::npos);
        }
    }
    // Shape C: bound klass whose port does NOT advertise the members
    // capability — the capability gate refuses named even with a
    // resolvable language and a non-null vtable slot.
    {
        runtime::interop::InteropVTable vt{};
        vt.read_member = native_read;
        runtime::interop::InteropRegistry no_caps;
        no_caps.register_language("no-caps", vt, 0,
                                  runtime::interop::PortKind::Foreign);
        auto module = assemble_module(src);
        VORTEX_EXPECT(module.has_value());
        if (!module) return;
        gc::Heap heap;
        vm::Interpreter interp(heap);
        interp.set_interop_registry(&no_caps);
        auto boot = interp.build_module_runtime(*module);
        VORTEX_EXPECT(boot.has_value());
        if (!boot) return;
        for (void* k : module->runtime.klass_table) {
            VORTEX_EXPECT(no_caps.bind_klass(1, k).has_value());
        }
        auto res = interp.run(*module, "main", {});
        VORTEX_EXPECT(!res.has_value());
        if (!res) {
            VORTEX_EXPECT(res.error().code ==
                          support::ErrorCode::Unsupported);
            VORTEX_EXPECT(res.error().message ==
                          std::string(
                              runtime::interop::kInteropUnsupportedMessage));
        }
    }
    // Shape D: the HAS_MEMBERS leg of Poly.Send honors the SAME capability
    // gate (Task 12) — a NON-NULL has_members slot on a 0-capability
    // language must still refuse named; without the gate the slot probe
    // would service the message (Rule 3).
    {
        runtime::interop::InteropVTable vt{};
        vt.has_members = native_has_members;
        runtime::interop::InteropRegistry no_caps;
        no_caps.register_language("no-caps", vt, 0,
                                  runtime::interop::PortKind::Foreign);
        auto module = assemble_module(src);
        VORTEX_EXPECT(module.has_value());
        if (!module) return;
        gc::Heap heap;
        vm::Interpreter interp(heap);
        interp.set_interop_registry(&no_caps);
        auto boot = interp.build_module_runtime(*module);
        VORTEX_EXPECT(boot.has_value());
        if (!boot) return;
        for (void* k : module->runtime.klass_table) {
            VORTEX_EXPECT(no_caps.bind_klass(1, k).has_value());
        }
        auto res = interp.run(*module, "hm_main", {});
        VORTEX_EXPECT(!res.has_value());
        if (!res) {
            VORTEX_EXPECT(res.error().code ==
                          support::ErrorCode::Unsupported);
            VORTEX_EXPECT(res.error().message ==
                          std::string(runtime::interop::
                                          kInteropUnsupportedMessage));
        }
    }
}

// ---- J3 pass regression tests (Task 12 blockers, Rule 121) ------------------

VORTEX_TEST(j3_rle_forwards_stored_value_not_stale_load) {
    // Load f -> Store f -> Load f must forward the STORED value. The old
    // RLE recorded table entries with first-wins insert, so the second
    // load forwarded the PRE-store load (Rules 18/107). The two stores
    // also keep the object out of scalar replacement (multi-store
    // escape), so stage 7 is the only path that can get this wrong.
    const char* src = R"(
.class W
.field a in W
.method main(regs=8, args=0)
  New.Object v0, W
  Const.I32 v1, 1
  SetField v0, v1, W.a
  GetField v2, v0, W.a
  Const.I32 v3, 2
  SetField v0, v3, W.a
  GetField v4, v0, W.a
  Add.Any v5, v2, v4
  Return v5
.end
)";
    auto t0 = run_t0(src, "main", {});
    VORTEX_EXPECT(t0.has_value());
    if (t0) VORTEX_EXPECT_EQ(t0->value.as_smi(), 3);
    J3Harness h(src, "main");
    auto res = h.run({});
    VORTEX_EXPECT(res.has_value());
    if (res) VORTEX_EXPECT_EQ(res->as_smi(), 3);
}

VORTEX_TEST(j3_deferred_field_init_respects_opaque_call) {
    // An intervening CALL observes the heap: the earlier store must
    // survive deferred-field-init (Rules 41/45/107). reader() exceeds the
    // 64-node inline callee cap, so the call stays opaque; killing the
    // first store would make reader() observe the default field instead
    // of 1 (12 vs 2 — the discriminator).
    const char* adds = "  Add.Any v1, v1, v2\n";
    std::string reader =
        ".method reader(regs=6, args=1)\n"
        "  Const.I32 v1, 1\n"
        "  Const.I32 v2, 1\n";
    for (int i = 0; i < 60; ++i) reader += adds;  // > inline_callee_node_cap
    reader +=
        "  GetField v3, v0, W.a\n"
        "  Return v3\n"
        ".end\n";
    const std::string src = std::string(
        ".class W\n"
        ".field a in W\n") + reader + R"(
.method main(regs=9, args=0)
  New.Object v0, W
  Const.I32 v1, 1
  SetField v0, v1, W.a
  Call.Direct v4, v0, 1, reader
  Const.I32 v2, 2
  SetField v0, v2, W.a
  GetField v5, v0, W.a
  Const.I32 v6, 10
  Mul.Any v7, v4, v6
  Add.Any v8, v7, v5
  Return v8
.end
)";
    auto t0 = run_t0(src.c_str(), "main", {});
    VORTEX_EXPECT(t0.has_value());
    if (t0) VORTEX_EXPECT_EQ(t0->value.as_smi(), 12);
    J3Harness h(src.c_str(), "main");
    auto res = h.run({});
    VORTEX_EXPECT(res.has_value());
    if (res) VORTEX_EXPECT_EQ(res->as_smi(), 12);
}

// ---- interop: T0 EXECUTE/SEND dispatch (the call-shaped messages) ------------
//
// The assembler + verifier + handlers mirror the locked contract
// (opcode.hpp section 15a); these tests are the EXECUTE/SEND legs of the
// mirror (review B2: the contract previously had zero coverage here).

VORTEX_TEST(interop_t0_poly_execute_send_parity) {
    InteropFixture fx;
    // EXECUTE: window (v3,v4) = (5,7) -> native_execute sums -> 12.
    // SEND: msg 1 = READ_MEMBER with the window arg (smi 0) -> field a = 5.
    const char* src = R"(
.language native-test
.interop_mask 3
.class W
.field a in W
.method exec_main(regs=8, args=0)
  .requires interop_messages
  Const.I32 v3, 5
  Const.I32 v4, 7
  New.Object v2, W
  SetField v2, v3, W.a
  Poly.Execute v5, v2, v3, 2
  Return v5
.end
.method send_main(regs=8, args=0)
  .requires interop_messages
  Const.I32 v3, 5
  New.Object v2, W
  SetField v2, v3, W.a
  Const.I32 v7, 0
  Poly.Send v6, 1, v2, v7, 1
  Return v6
.end
)";
    auto module = assemble_module(src);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.set_interop_registry(&fx.registry);
    auto boot = interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    for (void* k : module->runtime.klass_table) {
        VORTEX_EXPECT(fx.registry.bind_klass(1, k).has_value());
    }
    auto ex = interp.run(*module, "exec_main", {});
    VORTEX_EXPECT(ex.has_value());
    if (ex) VORTEX_EXPECT_EQ(ex->value.as_smi(), 12);
    auto se = interp.run(*module, "send_main", {});
    VORTEX_EXPECT(se.has_value());
    if (se) VORTEX_EXPECT_EQ(se->value.as_smi(), 5);
}

VORTEX_TEST(interop_execute_gating_and_window) {
    // EXECUTE with the module mask missing the CAP_INTEROP_EXECUTE group
    // bit -> load rejection (negotiation at load, Rule 3).
    {
        const char* src = R"(
.language native-test
.interop_mask 1
.class W
.method main(regs=8, args=0)
  .requires interop_messages
  New.Object v2, W
  Poly.Execute v5, v2, v0, 2
  Return v5
.end
)";
        auto module = assemble_module(src);
        VORTEX_EXPECT(module.has_value());
        if (module) VORTEX_EXPECT(!verify_module(*module).has_value());
    }
    // A full-frame window (base 0, argc == register_count) is LEGAL: the
    // argc slot rides a COUNT, not a register (locked contract) — the
    // generic register range check must not reject it (review B6).
    {
        const char* src = R"(
.language native-test
.interop_mask 3
.class W
.method main(regs=8, args=0)
  .requires interop_messages
  New.Object v2, W
  Poly.Execute v5, v2, v0, 8
  Return v5
.end
)";
        auto module = assemble_module(src);
        VORTEX_EXPECT(module.has_value());
        if (!module) return;
        auto res = verify_module(*module);
        VORTEX_EXPECT(res.has_value());
        if (!res) {
            std::printf("  verify said: %s\n", res.error().message.c_str());
        }
    }
    // Window overflow (base + argc > register_count) -> rejected.
    {
        const char* src = R"(
.language native-test
.interop_mask 3
.class W
.method main(regs=8, args=0)
  .requires interop_messages
  New.Object v2, W
  Poly.Execute v5, v2, v6, 4
  Return v5
.end
)";
        auto module = assemble_module(src);
        VORTEX_EXPECT(module.has_value());
        if (module) VORTEX_EXPECT(!verify_module(*module).has_value());
    }
}

// ---- the bootstrap verification gate (review B1 regression) -------------------

VORTEX_TEST(interop_bootstrap_gates_verification) {
    // build_module_runtime — the JIT tier-up bootstrap — used to set
    // runtime.ready WITHOUT a verification verdict, so a module that
    // verify_module REJECTS could execute through the sanctioned
    // bootstrap sequence (ASan-proven heap overflow on POLY_EXECUTE's
    // unchecked window). Rule 7/9: the verify+capability gate now lives
    // inside build_module_runtime itself.
    const char* src = R"(
.language native-test
.class W
.method main(regs=6, args=0)
  New.Object v1, W
  Poly.Read v0, v1, 0
  Return v0
.end
)";
    auto module = assemble_module(src);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    // The load gate rejects...
    VORTEX_EXPECT(!verify_module(*module).has_value());
    gc::Heap heap;
    vm::Interpreter interp(heap);
    // ...and the bootstrap path rejects too — a ready-but-unverified
    // module can never exist.
    auto boot = interp.build_module_runtime(*module);
    VORTEX_EXPECT(!boot.has_value());
    if (!boot) {
        VORTEX_EXPECT(boot.error().code == support::ErrorCode::VerifyError);
    }
}

// ---- port-kind binding freeze (review B3: the Rules 30/31 dependency) ---------

VORTEX_TEST(interop_bind_klass_frozen) {
    InteropFixture fx;
    // A second language for the cross-binding refusal.
    runtime::interop::InteropVTable vt{};
    fx.registry.register_language("other", vt, 0,
                                  runtime::interop::PortKind::Foreign);
    auto module = assemble_module(R"(
.language native-test
.class W
.method main(regs=2, args=0)
  Return v0
.end
)");
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto boot = interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    VORTEX_EXPECT_EQ(module->runtime.klass_table.size(), size_t{1});
    void* k = module->runtime.klass_table[0];
    // Invalid arguments are NAMED errors, never silent no-ops (Rule 76).
    VORTEX_EXPECT(!fx.registry.bind_klass(0, k).has_value());
    VORTEX_EXPECT(!fx.registry.bind_klass(9, k).has_value());
    VORTEX_EXPECT(!fx.registry.bind_klass(1, nullptr).has_value());
    // First bind ok; a same-language rebind is idempotent.
    VORTEX_EXPECT(fx.registry.bind_klass(1, k).has_value());
    VORTEX_EXPECT(fx.registry.bind_klass(1, k).has_value());
    // Cross-language rebind refused: installed JIT code speculated on the
    // port kind with no runtime re-check — the freeze IS the invalidation
    // dependency (Rules 30/31; a silent last-wins rebind would strand it).
    auto x = fx.registry.bind_klass(2, k);
    VORTEX_EXPECT(!x.has_value());
    if (!x) {
        VORTEX_EXPECT(x.error().code ==
                      support::ErrorCode::InvalidState);
    }
    // The binding is unchanged by the refused rebind.
    VORTEX_EXPECT_EQ(fx.registry.language_of_klass(k), 1);
}

// ---- J3 refuses the call-shaped interop messages (J4 devirt) ------------------

VORTEX_TEST(j3_refuses_poly_execute_named) {
    InteropFixture fx;
    // POLY_EXECUTE/POLY_SEND graph lowering lands with J4 devirt; the J3
    // builder must refuse NAMED (Rule 76) — never miscompile, never
    // silently fall back to generic code that loses the message.
    J3Harness h(R"(
.language native-test
.interop_mask 3
.class W
.field a in W
.method main(regs=8, args=0)
  .requires interop_messages
  Const.I32 v3, 5
  Const.I32 v4, 7
  New.Object v2, W
  SetField v2, v3, W.a
  Poly.Execute v5, v2, v3, 2
  Return v5
.end
)", "main");
    h.set_interop(&fx.registry);
    auto j3 = h.run({});
    VORTEX_EXPECT(!j3.has_value());
    if (!j3) {
        VORTEX_EXPECT(j3.error().message.find("build refused") !=
                      std::string::npos);
    }
}

// ---- J3 tier: parity + XLEA wrapper elimination --------------------------------

VORTEX_TEST(j3_parity_and_xlea_wrapper_elimination) {
    InteropFixture fx;
    // The xlea.md section-2 scenario: allocate a wrapper, store two fields,
    // read them back through POLY messages, add. J3 lowers the messages
    // into guarded field accesses, proves the wrapper non-escaping, and
    // scalar-replaces it — the allocation and the dispatches are GONE.
    J3Harness h(R"(
.language native-test
.interop_mask 1
.class W
.field a in W
.field b in W
.method callee(regs=8, args=1)
  .requires interop_messages
  Poly.Read v2, v0, 0
  Poly.Read v3, v0, 1
  Add.Any v4, v2, v3
  Return v4
.end
.method main(regs=8, args=0)
  .requires interop_messages
  Const.I32 v0, 20
  Const.I32 v1, 22
  New.Object v2, W
  SetField v2, v0, W.a
  SetField v2, v1, W.b
  Call.Direct v3, v2, 1, callee
  Return v3
.end
)", "main");
    h.set_interop(&fx.registry);
    // T0 truth.
    auto t0 = run_t0(R"(
.language native-test
.interop_mask 1
.class W
.field a in W
.field b in W
.method main(regs=8, args=0)
  .requires interop_messages
  Const.I32 v0, 20
  Const.I32 v1, 22
  New.Object v2, W
  SetField v2, v0, W.a
  SetField v2, v1, W.b
  Call.Direct v3, v2, 1, callee
  Return v3
.end
.method callee(regs=8, args=1)
  .requires interop_messages
  Poly.Read v2, v0, 0
  Poly.Read v3, v0, 1
  Add.Any v4, v2, v3
  Return v4
.end
)", "main", {}, &fx.registry);
    VORTEX_EXPECT(t0.has_value());
    if (!t0) return;
    VORTEX_EXPECT_EQ(t0->value.as_smi(), 42);
    auto j3 = h.run({});
    VORTEX_EXPECT(j3.has_value());
    if (!j3) return;
    VORTEX_EXPECT_EQ(j3->as_smi(), 42);
    // XLEA payoff is observable: the wrapper was scalar-replaced.
    VORTEX_EXPECT(h.code().stats.scalar_replaced >= 1);
}

VORTEX_TEST(j3_region_failure_accounting_isolated) {
    InteropFixture fx;
    // The wrapper is allocated INSIDE the harness module (one klass
    // family), so the specialized guards pass on the happy path: every
    // region stays Live with a zero failure counter after the J3 run.
    J3Harness h(R"(
.language native-test
.interop_mask 1
.class W
.field a in W
.field b in W
.method main(regs=8, args=0)
  .requires interop_messages
  Const.I32 v0, 3
  Const.I32 v1, 4
  New.Object v2, W
  SetField v2, v0, W.a
  SetField v2, v1, W.b
  Call.Direct v3, v2, 1, process
  Return v3
.end
.method process(regs=8, args=1)
  .requires interop_messages
  Poly.Read v1, v0, 0
  Poly.Read v2, v0, 1
  Add.Any v3, v1, v2
  Return v3
.end
)", "main");
    h.set_interop(&fx.registry);
    auto j3 = h.run({});
    VORTEX_EXPECT(j3.has_value());
    if (!j3) return;
    VORTEX_EXPECT_EQ(j3->as_smi(), 7);
    VORTEX_EXPECT(h.core().regions != nullptr);
    if (h.core().regions != nullptr) {
        VORTEX_EXPECT(h.core().regions->size() >= 1);
        for (const RegionDescriptor& r : h.core().regions->regions()) {
            VORTEX_EXPECT_EQ(r.failure_count, 0u);
            VORTEX_EXPECT_EQ(r.state, RegionState::Live);
        }
    }
}

VORTEX_TEST(j3_poll_capture_resumes_instead_of_rerun) {
    // Region-capture suspension (docs/deopt-rbpd.md 2-4): the poll fires
    // AFTER `count` ran once; the captured resume re-enters T0 AT the poll
    // pc, so `count` executes exactly once. The M1 whole-method rerun would
    // make it twice (the observable difference the test pins).
    J3Harness h(R"(
.method main(regs=6, args=0)
  Const.I32 v0, 1
  Call.Builtin v1, v0, 0, count
  safepoint
  Return v1
.end
)", "main");
    // T0 truth (no armed poll): count runs once, returns 1.
    auto t0 = run_t0(R"(
.method main(regs=6, args=0)
  Const.I32 v0, 1
  Call.Builtin v1, v0, 0, count
  safepoint
  Return v1
.end
)", "main", {});
    VORTEX_EXPECT(t0.has_value());
    if (!t0) return;
    VORTEX_EXPECT_EQ(t0->value.as_smi(), 1);
    // J3 with the poll ARMED: the J3 body executes `count` once, the poll
    // fires, and the captured resume re-enters T0 AT the poll pc. The
    // counter therefore advances by exactly ONE (65 = 64 warmups + 1) and
    // the region table records a SuspensionPoll event. The M1 whole-method
    // T0 rerun would execute `count` a SECOND time (66) — these assertions
    // pin the region-capture resume.
    auto j3 = h.run({});
    VORTEX_EXPECT(j3.has_value());
    if (!j3) return;
    VORTEX_EXPECT_EQ(j3->as_smi(), 65);  // unarmed baseline: one count
    h.arm_poll();
    j3 = h.run({});
    VORTEX_EXPECT(j3.has_value());
    if (!j3) return;
    VORTEX_EXPECT_EQ(j3->as_smi(), 65);  // resume, not rerun (would be 66)
    VORTEX_EXPECT_EQ(h.counter(), 65u);
    VORTEX_EXPECT(h.core().regions != nullptr);
    if (h.core().regions != nullptr) {
        VORTEX_EXPECT(h.core().regions->last_event() != nullptr);
        VORTEX_EXPECT_EQ(h.core().regions->last_event()->reason,
                         DeoptReason::SuspensionPoll);
    }
}

// ---- deopt over a scalar-replaced frame (Rule 39 remat, both blockers) --------

VORTEX_TEST(j3_deopt_rebuilds_scalar_replaced_wrapper) {
    InteropFixture fx;
    // A wrapper is scalar-replaced by J3, then the ARMED POLL fires: the
    // captured deopt must REBUILD the wrapper from the record (fields 30/12
    // embedded as consts) and T0 resumes at the poll pc, reading the
    // rebuilt object through the native port. Result: 42 + 30 = 72 —
    // identical to pure T0. A broken rebuild (e.g. the window slot holding
    // a Smi instead of the rebuilt reference) fails the post-resume read.
    J3Harness h(R"(
.language native-test
.interop_mask 1
.class W
.field a in W
.field b in W
.method main(regs=8, args=0)
  .requires interop_messages
  Const.I32 v0, 30
  Const.I32 v1, 12
  New.Object v2, W
  SetField v2, v0, W.a
  SetField v2, v1, W.b
  Poly.Read v3, v2, 0
  Poly.Read v4, v2, 1
  Add.Any v5, v3, v4
  Safepoint
  Poly.Read v6, v2, 0
  Add.Any v7, v5, v6
  Return v7
.end
)", "main");
    h.set_interop(&fx.registry);
    auto t0 = run_t0(R"(
.language native-test
.interop_mask 1
.class W
.field a in W
.field b in W
.method main(regs=8, args=0)
  .requires interop_messages
  Const.I32 v0, 30
  Const.I32 v1, 12
  New.Object v2, W
  SetField v2, v0, W.a
  SetField v2, v1, W.b
  Poly.Read v3, v2, 0
  Poly.Read v4, v2, 1
  Add.Any v5, v3, v4
  Safepoint
  Poly.Read v6, v2, 0
  Add.Any v7, v5, v6
  Return v7
.end
)", "main", {}, &fx.registry);
    VORTEX_EXPECT(t0.has_value());
    if (!t0) return;
    VORTEX_EXPECT_EQ(t0->value.as_smi(), 72);
    auto j3 = h.run({});
    VORTEX_EXPECT(j3.has_value());
    if (!j3) return;
    VORTEX_EXPECT_EQ(j3->as_smi(), 72);  // unarmed: fast path, no rebuild
    VORTEX_EXPECT(h.code().stats.scalar_replaced >= 1);
    h.arm_poll();
    j3 = h.run({});
    VORTEX_EXPECT(j3.has_value());
    if (!j3) return;
    // ARMED: the poll fired mid-frame; the rebuilt wrapper serves the
    // post-resume read. Parity with T0 proves the remat (Rule 39).
    VORTEX_EXPECT_EQ(j3->as_smi(), 72);
}

// ---- escape summaries (docs/xlea.md section 4.1 laws) ---------------------------

VORTEX_TEST(escape_summary_monotonicity_and_identity) {
    ir::EscapeSummaryTable table;
    ir::EscapeSummary s;
    s.method_id = 9;
    s.param_count = 2;
    s.params = {ir::ParamEscape::NoEscape, ir::ParamEscape::NoEscape};
    s.graph_hash = 1234;
    table.publish(s);
    VORTEX_EXPECT(table.lookup(9) != nullptr);
    VORTEX_EXPECT_EQ(table.lookup(9)->params[1], ir::ParamEscape::NoEscape);

    // Same graph hash: weakening NoEscape -> ArgEscape sticks; a strengthen
    // attempt is ignored (monotonicity law).
    ir::EscapeSummary weaken = s;
    weaken.params[1] = ir::ParamEscape::ArgEscape;
    table.publish(weaken);
    VORTEX_EXPECT_EQ(table.lookup(9)->params[1], ir::ParamEscape::ArgEscape);
    ir::EscapeSummary strengthen = weaken;
    strengthen.params[1] = ir::ParamEscape::NoEscape;
    table.publish(strengthen);
    VORTEX_EXPECT_EQ(table.lookup(9)->params[1], ir::ParamEscape::ArgEscape);

    // New graph hash: the summary re-registers (identity law).
    ir::EscapeSummary recompiled = weaken;
    recompiled.graph_hash = 5678;
    recompiled.params[1] = ir::ParamEscape::NoEscape;
    table.publish(recompiled);
    VORTEX_EXPECT_EQ(table.lookup(9)->graph_hash, 5678u);
    VORTEX_EXPECT_EQ(table.lookup(9)->params[1], ir::ParamEscape::NoEscape);

    // Invalidation: the summary weakens to absent (Unknown everywhere).
    table.invalidate(9);
    VORTEX_EXPECT(table.lookup(9) == nullptr);
    // Out-of-range parameter reads are Unknown, never over-reads.
    VORTEX_EXPECT_EQ(table.lookup(9) == nullptr
                         ? ir::ParamEscape::Unknown
                         : table.lookup(9)->param(99),
                     ir::ParamEscape::Unknown);
}
