// CEP:FILE: tests/test_ffi.cpp
// CEP:WHAT: Infra 5 FFI test suite (M6-c, docs/infrastructure/05-ffi.md DoD
//           1-9): golden machine-code pins + determinism for both stub
//           directions, guest-to-native parity (smi add, F64 payload bits,
//           pointer identity, bool/void), boundary failure pins (type error
//           skips the call; int63 overflow records the named bit), boundary
//           safepoint tick pins, native-to-guest callback parity through a
//           real T0 re-entry, callback bool/null argument normalization and
//           named error recording, emit refusals, the mixed-stack walk
//           through a real J1 published frame + trampoline + native frames,
//           the Rule-132 exception bridge, and registry pairing.
// CEP:WHY: The FFI boundary is machine code — every translation rule needs
//           a pin that fails loudly when the emission contract drifts
//           (the M1 golden-pin precedent).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg; a stub fault surfaces as
//           a process-level signal in CI (no silent pass).
// CEP:ASSUMES: Linux x86-64 (the stubs are SysV machine code); frame
//           pointers in every target (the mixed walk crosses harness
//           frames); the J1 walk leg reuses the test_j1 harness shape.
// CEP:COST: @cold test process.
// CEP:EVIDENCE: this file (each test names its DoD item).
// Infra 5 FFI tests (M6-c, docs/infrastructure/05-ffi.md DoD 1-9):
//   - golden machine-code pins (both stub directions) + determinism
//   - guest→native parity: smi add, f64 payload bits, ptr identity, bool/void
//   - boundary failures: type error, int63 return overflow (named bits)
//   - boundary safepoint: armed tick / disarmed silence
//   - native→guest callback: engine re-entry parity, bool/null args,
//     arg-overflow + return-type-error recording
//   - emit refusals: arity caps, F64/Ptr callback args, Null call-out, abi
//   - mixed walk: J1 published frame + trampoline + native frames in ONE walk
//   - exception bridge: named ForeignBoundary failures (Rule 132)
//   - registry pairing: register on emit, unregister on destroy
#include "vortex_test.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "vortex/gc/icggc.hpp"
#include "vortex/infra/ffi.hpp"
#include "vortex/infra/threading.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j1/stencil_corpus.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;
using namespace vortex::infra;
using namespace vortex::j1;
using vortex::support::ErrorCode;

namespace {

// CEP:WHAT: make_sig — builds a ForeignSignature from an ordered kind list.
// CEP:WHY: Every stub test spells its signature through this one helper so the pins stay comparable.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — test-side scaffolding.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: consumed by the tests below.
ForeignSignature make_sig(std::vector<ForeignArgKind> args,
                          ForeignReturnKind ret) {
    ForeignSignature s;
    s.args = std::move(args);
    s.return_kind = ret;
    return s;
}

// ---- foreign callees (extern "C": the stubs call plain SysV functions) ----

extern "C" int64_t ffi_test_add(int64_t a, int64_t b) { return a + b; }
extern "C" int64_t ffi_test_sum6(int64_t a, int64_t b, int64_t c, int64_t d,
                                 int64_t e, int64_t f) {
    return a + b + c + d + e + f;
}
extern "C" int64_t ffi_test_bool_and(int64_t a, int64_t b) {
    return (a != 0 && b != 0) ? 1 : 0;
}
// CEP:WHAT: ffi_test_nop — empty foreign callee (Void-return paths).
// CEP:WHY: DoD 2: void returns map to the canonical undefined word.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — test-side scaffolding.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: consumed by the tests below.
extern "C" void ffi_test_nop(int64_t) { return; }
// CEP:WHAT: ffi_test_ident — identity foreign callee (Ptr round trips).
// CEP:WHY: DoD 2: pointer unboxing preserves the 16-byte-aligned base.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — test-side scaffolding.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: consumed by the tests below.
extern "C" void* ffi_test_ident(void* p) { return p; }

struct ProbeState {
    uint64_t called = 0;
    int64_t last_a = 0;
    int64_t last_b = 0;
    uint64_t dbl_bits_a = 0;
    uint64_t dbl_bits_b = 0;
    void* last_ptr = nullptr;
    std::vector<UnifiedFrame> walk;
};
// CEP:WHAT: probe() — shared probe state for the recording foreign callees.
// CEP:WHY: The stubs carry no user channel on the foreign side; the probes park observations in process-static state the tests reset and assert.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — test-side scaffolding.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: consumed by the tests below.
ProbeState& probe() {
    static ProbeState s;
    return s;
}

// CEP:WHAT: ffi_test_probe — records call count and arguments, returns a+b.
// CEP:WHY: The boundary tests assert on callee-observed values, not stub outputs alone (the callee is the ground truth of the translation).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — test-side scaffolding.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: consumed by the tests below.
extern "C" int64_t ffi_test_probe(int64_t a, int64_t b) {
    auto& p = probe();
    ++p.called;
    p.last_a = a;
    p.last_b = b;
    return a + b;
}
// CEP:WHAT: ffi_test_dprobe — records raw payload bits of both double arguments.
// CEP:WHY: DoD 3: the F64 arrival check is bit-exact, not value-equal.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — test-side scaffolding.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: consumed by the tests below.
extern "C" double ffi_test_dprobe(double a, double b) {
    auto& p = probe();
    ++p.called;
    std::memcpy(&p.dbl_bits_a, &a, sizeof(a));
    std::memcpy(&p.dbl_bits_b, &b, sizeof(b));
    return a + b;
}
// CEP:WHAT: ffi_test_pprobe — records the unboxed pointer.
// CEP:WHY: DoD 2: the Ptr translation must deliver the object base itself.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — test-side scaffolding.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: consumed by the tests below.
extern "C" void* ffi_test_pprobe(void* p) {
    probe().last_ptr = p;
    return p;
}
// CEP:WHAT: ffi_test_walk_site — captures the mixed walk from inside the foreign call.
// CEP:WHY: DoD 6: the stack sample must be taken while the trampoline and JIT frames are live, i.e. inside the callee the stub called.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — test-side scaffolding.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: consumed by the tests below.
extern "C" int64_t ffi_test_walk_site(int64_t, int64_t) {
    probe().walk = UnifiedUnwinder::walk_current();
    return 0;
}

// ---- callback shim: re-enters the engine (T0) ------------------------------

struct ShimEnv {
    ugb::UGBModule* module = nullptr;
    vm::Interpreter* interp = nullptr;
    const char* entry = nullptr;
    uint64_t seen_words[8] = {0};
    size_t seen_n = 0;
    uint64_t forced_return = 0;
    bool force = false;
};

// CEP:WHAT: ffi_test_shim — embedder shim: records tagged words, re-enters the engine (T0 run) or echoes a forced word.
// CEP:WHY: DoD 4: the callback parity leg requires a shim that runs the REAL engine, not a stub of it.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — test-side scaffolding.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: consumed by the tests below.
extern "C" uint64_t ffi_test_shim(const TaggedValue* args, size_t n,
                                  void* user) {
    auto* env = static_cast<ShimEnv*>(user);
    env->seen_n = n;
    for (size_t i = 0; i < n && i < 8; ++i) env->seen_words[i] = args[i].raw();
    if (env->force) return env->forced_return;
    auto r = env->interp->run(*env->module, env->entry,
                              std::span<const TaggedValue>(args, n));
    return r ? r->value.raw() : TaggedValue::undefined().raw();
}

// ---- shared J1 machinery (mirrors the test_j1 parity harness) --------------

// CEP:WHAT: make_corpus — assembles the default J1 stencil corpus (test_j1 shape).
// CEP:WHY: The walk leg compiles real J1 code, so it needs the real corpus.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — test-side scaffolding.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: consumed by the tests below.
StencilTable make_corpus() {
    StencilTable t;
    build_default_corpus(t);
    return t;
}

// The J1 source for the mixed-walk test: one method calling the probe
// builtin, which itself drives the guest→native stub from inside the J1
// frame — the walk then names [native..., trampoline, native..., JIT, ...].
const char* kWalkerSrc =
    ".method walker(regs=4, args=1)\n"
    "    Call.Builtin v2, v0, 1, ffi_probe\n"
    "    Return v2\n"
    ".end\n";

}  // namespace

// ---- golden pins ------------------------------------------------------------
// The pins below are byte-exact dumps of the two stub directions for fixed
// signatures (deterministic-compile DoD 1): identical inputs MUST produce
// identical bytes — the pin fails loudly if any emission change lands
// without a deliberate re-pin (the M1 golden-pin precedent).

// CEP:WHAT: Test: one walk names [native..., trampoline, native..., JIT (J1), ...] through a real published frame.
// CEP:WHY: DoD 6 (the mixed-walk shape end to end).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_mixed_walk_j1_trampoline_native) {
    StencilTable corpus = make_corpus();
    auto module = ugb::assemble_module(kWalkerSrc);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);

    // The builtin drives the guest→native stub FROM inside the J1 frame.
    auto stub_holder =
        TrampolineGenerator::emit_guest_to_native(
            make_sig({ForeignArgKind::Int, ForeignArgKind::Int},
                     ForeignReturnKind::Int),
            reinterpret_cast<void*>(&ffi_test_walk_site), nullptr);
    VORTEX_EXPECT(stub_holder.has_value());
    if (!stub_holder) return;
    GuestToNativeStub stub = std::move(*stub_holder);

    interp.register_builtin(
        "ffi_probe", [](std::span<const TaggedValue> args, void* user) {
            auto* s = static_cast<GuestToNativeStub*>(user);
            return s->entry(args.data(), &s->ctx);
        }, &stub);

    auto boot = interp.build_module_runtime(*module);
    VORTEX_EXPECT(boot.has_value());
    if (!boot) return;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, *module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    const int32_t id = module->find_method("walker");
    VORTEX_EXPECT(id >= 0);
    if (id < 0) return;
    j1::BaselineJit baseline(corpus);
    j1::BaselineJob job;
    job.module = &*module;
    job.method_id = static_cast<uint32_t>(id);
    job.klass_addrs = &bindings.klass_addr_table;
    job.field_offsets = bindings.field_offsets.data();
    job.field_offset_count = bindings.field_offsets.size();
    job.double_klass = bindings.double_klass;
    auto code = baseline.compile(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    auto ex = j1::publish_baseline(*code);
    VORTEX_EXPECT(ex.has_value());
    if (!ex) return;

    probe().walk.clear();
    const std::vector<TaggedValue> walker_args = {TaggedValue::smi(1)};
    auto run = j1::run_baseline(*ex, bindings, walker_args);
    VORTEX_EXPECT(run.has_value());
    if (!run) return;
    VORTEX_EXPECT_EQ(run->as_smi(), 0);  // walk site returns 0

    const std::vector<UnifiedFrame>& frames = probe().walk;
    VORTEX_EXPECT(frames.size() >= 4);
    // walk_current frame 0 names the capture site's caller (the walk-site
    // foreign fn): a Native frame, not the trampoline (M6-c review R2-7c).
    VORTEX_EXPECT(frames[0].kind == UnifiedFrame::Kind::Native);
    // Shape: [native walk site, TRAMPOLINE, native helpers..., JIT (J1),
    //         native driver...]
    size_t tramp = SIZE_MAX;
    size_t jit = SIZE_MAX;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].kind == UnifiedFrame::Kind::Trampoline && tramp == SIZE_MAX)
            tramp = i;
        if (frames[i].kind == UnifiedFrame::Kind::Jit && jit == SIZE_MAX)
            jit = i;
    }
    VORTEX_EXPECT(tramp != SIZE_MAX);
    VORTEX_EXPECT(jit != SIZE_MAX);
    VORTEX_EXPECT(tramp < jit);  // the trampoline sits BELOW the J1 frame
    if (tramp != SIZE_MAX) {
        // The trampoline frame's pc offset is inside the stub's range.
        VORTEX_EXPECT(frames[tramp].pc_offset < 1024);
    }
    if (jit != SIZE_MAX) {
        // The JIT frame is a real published J1 record.
        const gc::GcCodeRecord* rec =
            gc::gc_code_registry().lookup(frames[jit].ip);
        VORTEX_EXPECT(rec != nullptr);
        if (rec) VORTEX_EXPECT(frames[jit].pc_offset < rec->code_bytes);
    }
}

// CEP:WHAT: Test: pins the guest-to-native stub's first 48 bytes (frame, magic, spills, poll load).
// CEP:WHY: DoD 1: the bytes are the emission contract; a change must re-pin deliberately.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_guest_to_native_golden_pin) {
    // Signature: Int(Int, Int) -> Int.
    TrampolineGenerator gen;
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Int, ForeignArgKind::Int},
                 ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_add), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);

    // Pinned prologue + poll prefix (first 48 bytes): frame, magic push
    // ('VFXF0123' little-endian), callee-saved spills, ctx parking, scratch
    // sub, poll-word load through the context. (Re-pin deliberately when
    // the emission contract changes — the bytes are the contract.)
    static const uint8_t kPin[48] = {
        0x55, 0x48, 0x89, 0xE5, 0x48, 0xB8, 0x33, 0x32, 0x31, 0x30, 0x46,
        0x58, 0x46, 0x56, 0x50, 0x53, 0x41, 0x54, 0x49, 0x89, 0xFC, 0x48,
        0x89, 0xF3, 0x48, 0x81, 0xEC, 0x18, 0x00, 0x00, 0x00, 0x48, 0x8B,
        0x43, 0x10, 0x48, 0x81, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x84,
        0x16, 0x00, 0x00, 0x00};
    VORTEX_EXPECT(std::memcmp(stub.memory.data(), kPin, sizeof(kPin)) == 0);
}

// CEP:WHAT: Test: pins the callback stub's first 40 bytes (frame, island load, arena sub, first spill).
// CEP:WHY: DoD 1: the bytes are the emission contract; a change must re-pin deliberately.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_native_to_guest_golden_pin) {
    // Signature: Int(Int, Int) -> Int callback.
    auto r = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::Int, ForeignArgKind::Int},
                 ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_shim), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    NativeToGuestStub stub = std::move(*r);

    // Pinned prologue prefix (first 48 bytes — DoD 1 parity with the
    // call-out pin): frame, magic, spills, the rip-relative island load
    // (48 8D 1D <disp32> 48 8B 1B — the disp32 is layout-only), arena sub,
    // both spill stores. The island CONTENT (8 bytes after the ret) is the
    // one allocation-dependent region and is deliberately outside the pin.
    static const uint8_t kPin[48] = {
        0x55, 0x48, 0x89, 0xE5, 0x48, 0xB8, 0x33, 0x32, 0x31, 0x30, 0x46,
        0x58, 0x46, 0x56, 0x50, 0x53, 0x41, 0x54, 0x48, 0x8D, 0x1D, 0xED,
        0x00, 0x00, 0x00, 0x48, 0x8B, 0x1B, 0x48, 0x81, 0xEC, 0x48, 0x00,
        0x00, 0x00, 0x49, 0x89, 0xE4, 0x49, 0x89, 0x7C, 0x24, 0x00, 0x49,
        0x89, 0x74, 0x24, 0x08};
    VORTEX_EXPECT(std::memcmp(stub.memory.data(), kPin, sizeof(kPin)) == 0);
}

// CEP:WHAT: Test: two emissions of one signature are byte-identical over the exact code length.
// CEP:WHY: DoD 1 deterministic-compile contract (no entropy, no baked addresses).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_emission_is_deterministic) {
    // DoD 1: two emissions of the same signature are byte-identical over
    // the EXACT code length (registry range covers it).
    const ForeignSignature s = make_sig(
        {ForeignArgKind::Int, ForeignArgKind::F64, ForeignArgKind::Ptr,
         ForeignArgKind::Bool},
        ForeignReturnKind::Int);
    auto a = TrampolineGenerator::emit_guest_to_native(
        s, reinterpret_cast<void*>(&ffi_test_add), nullptr);
    auto b = TrampolineGenerator::emit_guest_to_native(
        s, reinterpret_cast<void*>(&ffi_test_add), nullptr);
    VORTEX_EXPECT(a.has_value() && b.has_value());
    if (!a || !b) return;
    const auto ra = ffi_trampoline_registry().lookup(
        reinterpret_cast<uintptr_t>(a->memory.data()));
    const auto rb = ffi_trampoline_registry().lookup(
        reinterpret_cast<uintptr_t>(b->memory.data()));
    VORTEX_EXPECT(ra != nullptr && rb != nullptr);
    if (!ra || !rb) return;
    VORTEX_EXPECT(ra->hi - ra->lo == rb->hi - rb->lo);
    VORTEX_EXPECT(std::memcmp(a->memory.data(), b->memory.data(),
                              ra->hi - ra->lo) == 0);
    // Callback direction (M6-c review R1-5 + 17-c NEW-2): ALL code bytes
    // before the island (0..262) are allocation-independent; the island
    // CONTENT (the heap context pointer, 262..270) is the one
    // allocation-dependent region.
    auto c1 = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::Int, ForeignArgKind::Int},
                 ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_shim), nullptr);
    auto c2 = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::Int, ForeignArgKind::Int},
                 ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_shim), nullptr);
    VORTEX_EXPECT(c1.has_value() && c2.has_value());
    if (!c1 || !c2) return;
    const auto* r1 = ffi_trampoline_registry().lookup(
        reinterpret_cast<uintptr_t>(c1->memory.data()));
    const size_t code_end = r1 ? size_t(r1->hi - r1->lo) - 8 : 262;
    VORTEX_EXPECT(std::memcmp(c1->memory.data(), c2->memory.data(),
                              code_end) == 0);
    VORTEX_EXPECT(c1->memory.data()[262] != c2->memory.data()[262] ||
                  *reinterpret_cast<const uint64_t*>(c1->memory.data() + 262) !=
                      *reinterpret_cast<const uint64_t*>(c2->memory.data() +
                                                        262));
}

// ---- guest→native parity -----------------------------------------------------

// CEP:WHAT: Test: Int(Int,Int)->Int stub boxes the foreign sum exactly.
// CEP:WHY: DoD 2 (smi round trip through the boundary).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_guest_to_native_add_parity) {
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Int, ForeignArgKind::Int},
                 ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_add), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);
    const TaggedValue args[2] = {TaggedValue::smi(3), TaggedValue::smi(4)};
    const TaggedValue out = stub.entry(args, &stub.ctx);
    VORTEX_EXPECT_EQ(out.raw(), TaggedValue::smi(7).raw());
    VORTEX_EXPECT_EQ(stub.ctx.error, 0u);
}

// CEP:WHAT: Test: boxed-double arguments arrive at the foreign fn with exact payload bits via XMM.
// CEP:WHY: DoD 3 (negative + fractional payloads, bit-identical).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_guest_to_native_f64_payload_bits_exact) {
    gc::Heap heap;
    auto da = heap.allocate_double(-2.5);
    auto db = heap.allocate_double(0.875);
    auto dc = heap.allocate_double(1e-320);  // subnormal (DoD 3)
    VORTEX_EXPECT(da.has_value() && db.has_value() && dc.has_value());
    if (!da || !db || !dc) return;
    // Void return kind (M6-c review R2-2): the callee returns a double in
    // XMM0 — the Int return path would run the roundtrip on unspecified
    // RAX residue, asserting nothing. Void touches no return state.
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::F64, ForeignArgKind::F64},
                 ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_dprobe), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);
    const TaggedValue args[2] = {TaggedValue::heap_pointer(*da),
                                 TaggedValue::heap_pointer(*db)};
    probe().called = 0;
    const TaggedValue out = stub.entry(args, &stub.ctx);
    VORTEX_EXPECT_EQ(probe().called, 1u);
    VORTEX_EXPECT_EQ(stub.ctx.error, 0u);
    // Bit-exact payload arrival through XMM (negative + fractional).
    uint64_t ba = 0, bb = 0;
    const double x = -2.5, y = 0.875;
    std::memcpy(&ba, &x, 8);
    std::memcpy(&bb, &y, 8);
    VORTEX_EXPECT_EQ(probe().dbl_bits_a, ba);
    VORTEX_EXPECT_EQ(probe().dbl_bits_b, bb);
    (void)out;
    // Subnormal arrival (DoD 3): 1e-320 crosses bit-exact through XMM.
    probe().called = 0;
    const TaggedValue sc[2] = {TaggedValue::heap_pointer(*dc),
                               TaggedValue::heap_pointer(*dc)};
    (void)stub.entry(sc, &stub.ctx);
    uint64_t bc = 0;
    const double z = 1e-320;
    std::memcpy(&bc, &z, 8);
    VORTEX_EXPECT_EQ(probe().dbl_bits_a, bc);
    VORTEX_EXPECT_EQ(probe().dbl_bits_b, bc);
}

// CEP:WHAT: Test: Ptr arguments arrive as the 16-byte-aligned object base; Void returns undefined.
// CEP:WHY: DoD 2 (pointer unbox) + return-kind pin.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_guest_to_native_ptr_identity) {
    gc::Heap heap;
    auto obj = heap.allocate_double(1.0);
    VORTEX_EXPECT(obj.has_value());
    if (!obj) return;
    void* addr = static_cast<void*>(&**obj);
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Ptr}, ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_pprobe), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);
    probe().last_ptr = nullptr;
    const TaggedValue args[1] = {TaggedValue::heap_pointer(*obj)};
    const TaggedValue out = stub.entry(args, &stub.ctx);
    VORTEX_EXPECT(probe().last_ptr == addr);  // 16-byte-aligned base arrives
    VORTEX_EXPECT(out.raw() == TaggedValue::undefined().raw());
}

// CEP:WHAT: Test: canonical bool words normalize to native 0/1 in both directions.
// CEP:WHY: DoD 2 (Bool translation rule).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_guest_to_native_bool_and_null_arg_types) {
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Bool, ForeignArgKind::Bool},
                 ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_bool_and), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);
    const TaggedValue t[2] = {TaggedValue::boolean(true),
                              TaggedValue::boolean(false)};
    VORTEX_EXPECT_EQ(stub.entry(t, &stub.ctx).raw(),
                     TaggedValue::smi(0).raw());
    VORTEX_EXPECT_EQ(stub.ctx.error, 0u);
    const TaggedValue u[2] = {TaggedValue::boolean(true),
                              TaggedValue::boolean(true)};
    VORTEX_EXPECT_EQ(stub.entry(u, &stub.ctx).raw(),
                     TaggedValue::smi(1).raw());
}

// CEP:WHAT: Test: Void-returning foreign calls yield the canonical undefined word.
// CEP:WHY: DoD 2 (return-kind pin).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_guest_to_native_void_returns_undefined) {
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Int}, ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_nop), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);
    const TaggedValue args[1] = {TaggedValue::smi(9)};
    VORTEX_EXPECT(stub.entry(args, &stub.ctx).raw() ==
                  TaggedValue::undefined().raw());
    VORTEX_EXPECT_EQ(stub.ctx.error, 0u);
}

// CEP:WHAT: Test: the Bool-return call-out normalizes native 0/1 to the
//           canonical words (the R1-1 fix pin).
// CEP:WHY: The old emission validated canonical words — a 0/1-returning
//           callee recorded a phantom type error; this test pins the fix
//           and closes the coverage hole that hid it.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_guest_to_native_bool_return_normalization) {
    // M6-c review R1-1: the foreign side returns PLAIN 0/1 (the documented
    // native domain); the stub normalizes to the canonical words. The old
    // emission validated canonical words instead — a 0/1-returning callee
    // recorded a phantom type error. The Bool-return call-out path has its
    // own test since that hole.
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Bool, ForeignArgKind::Bool},
                 ForeignReturnKind::Bool),
        reinterpret_cast<void*>(&ffi_test_bool_and), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);
    const TaggedValue t[2] = {TaggedValue::boolean(true),
                              TaggedValue::boolean(true)};
    VORTEX_EXPECT(stub.entry(t, &stub.ctx).raw() ==
                  TaggedValue::boolean(true).raw());
    VORTEX_EXPECT_EQ(stub.ctx.error, 0u);
    const TaggedValue f[2] = {TaggedValue::boolean(true),
                              TaggedValue::boolean(false)};
    VORTEX_EXPECT(stub.entry(f, &stub.ctx).raw() ==
                  TaggedValue::boolean(false).raw());
    VORTEX_EXPECT_EQ(stub.ctx.error, 0u);
}

// CEP:WHAT: Test: the worst ACCEPTED shape (6 Int + 6 F64) emits within the
//           code budget and calls with exact parity (the R1-3 pin).
// CEP:WHY: The CEP:ASSUMES claims the budget covers the maximal shape —
//           this test makes that claim checked, and the emitter's named
//           budget failure turns overflow into a diagnosable reject.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_max_shape_emission_within_budget) {
    // M6-c review R1-3: the worst ACCEPTED shape (6 Int + 6 F64) emits and
    // calls; the budget check turns any overflow into a named failure, and
    // this test pins the headroom the CEP:ASSUMES claims.
    std::vector<ForeignArgKind> kinds;
    for (int i = 0; i < 6; ++i) kinds.push_back(ForeignArgKind::Int);
    for (int i = 0; i < 6; ++i) kinds.push_back(ForeignArgKind::F64);
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig(kinds, ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_sum6), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);
    std::vector<TaggedValue> args;
    for (int i = 0; i < 6; ++i) args.push_back(TaggedValue::smi(i + 1));
    gc::Heap heap;
    TaggedValue out = TaggedValue::undefined();
    // Six Int args reach the callee; the six F64 args need boxed doubles.
    for (int i = 0; i < 6; ++i) {
        auto d = heap.allocate_double(0.5);
        VORTEX_EXPECT(d.has_value());
        if (!d) return;
        args.push_back(TaggedValue::heap_pointer(*d));
    }
    out = stub.entry(args.data(), &stub.ctx);
    VORTEX_EXPECT_EQ(out.raw(), TaggedValue::smi(21).raw());  // 1+..+6
    VORTEX_EXPECT_EQ(stub.ctx.error, 0u);
}

// ---- boundary failures (named bits) ------------------------------------------

// CEP:WHAT: Test: a non-Smi word at an Int slot records the Type bit and the foreign callee never runs.
// CEP:WHY: DoD 2 named-failure semantics (Rule 82; no garbage reaches the callee).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_type_error_at_int_slot_skips_the_call) {
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Int, ForeignArgKind::Int},
                 ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_probe), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);
    probe().called = 0;
    const TaggedValue args[2] = {TaggedValue::null(), TaggedValue::smi(1)};
    const TaggedValue out = stub.entry(args, &stub.ctx);
    // Named failure: type bit set, canonical undefined word substituted,
    // and the foreign callee NEVER ran with a garbage argument.
    VORTEX_EXPECT((stub.ctx.error & kFfiErrType) != 0);
    VORTEX_EXPECT(out.raw() == TaggedValue::undefined().raw());
    VORTEX_EXPECT_EQ(probe().called, 0u);
}

// CEP:WHAT: Test: int63 return overflow records the Overflow bit; in-range extremes still box exactly.
// CEP:WHY: DoD 2 (roundtrip check at both bounds).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_return_overflow_records_named_bit) {
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Int, ForeignArgKind::Int},
                 ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_add), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);
    // 2^62 is one past smi_max: the roundtrip check must catch it.
    const TaggedValue args[2] = {TaggedValue::smi(1LL << 61),
                                 TaggedValue::smi(1LL << 61)};
    const TaggedValue out = stub.entry(args, &stub.ctx);
    VORTEX_EXPECT((stub.ctx.error & kFfiErrOverflow) != 0);
    VORTEX_EXPECT(out.raw() == TaggedValue::undefined().raw());
    // In-range maximum still boxes exactly.
    stub.ctx.error = 0;
    const TaggedValue ok[2] = {TaggedValue::smi((1LL << 62) - 1),
                               TaggedValue::smi(0)};
    VORTEX_EXPECT(stub.entry(ok, &stub.ctx).raw() ==
                  TaggedValue::smi((1LL << 62) - 1).raw());
    VORTEX_EXPECT_EQ(stub.ctx.error, 0u);
}

// CEP:WHAT: Test: armed poll word ticks the boundary hook exactly once per call; disarmed ticks zero.
// CEP:WHY: DoD 5 (boundary is the mutator's pause acknowledgment point).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_boundary_safepoint_ticks) {
    auto reserved = SafepointPollingPage::reserve();
    VORTEX_EXPECT(reserved.has_value());
    auto r = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Int}, ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_nop), nullptr);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    GuestToNativeStub stub = std::move(*r);
    const TaggedValue args[1] = {TaggedValue::smi(1)};

    // Disarmed: the default hook never fires.
    (void)SafepointPollingPage::disarm();
    stub.ctx.safepoint_ticks = 0;
    (void)stub.entry(args, &stub.ctx);
    VORTEX_EXPECT_EQ(stub.ctx.safepoint_ticks, 0u);

    // Armed: exactly one boundary acknowledgment per call.
    VORTEX_EXPECT(
        SafepointPollingPage::arm(SuspensionKind::Profiling).has_value());
    (void)stub.entry(args, &stub.ctx);
    VORTEX_EXPECT_EQ(stub.ctx.safepoint_ticks, 1u);
    (void)stub.entry(args, &stub.ctx);
    VORTEX_EXPECT_EQ(stub.ctx.safepoint_ticks, 2u);
    (void)SafepointPollingPage::disarm();
}

// ---- native→guest callback ----------------------------------------------------

// CEP:WHAT: Test: the callback stub re-enters the engine through the shim and matches the direct run.
// CEP:WHY: DoD 4 (engine re-entry parity + canonical tagged words at the shim).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_callback_reentry_parity) {
    const char* src =
        ".method add2(regs=4, args=2)\n"
        "    Add.I64 v0, v0, v1\n"
        "    Return v0\n"
        ".end\n";
    auto module = ugb::assemble_module(src);
    VORTEX_EXPECT(module.has_value());
    if (!module) return;
    gc::Heap heap;
    vm::Interpreter interp(heap);
    ShimEnv env;
    env.module = &*module;
    env.interp = &interp;
    env.entry = "add2";

    const std::vector<TaggedValue> direct_args = {TaggedValue::smi(5),
                                                  TaggedValue::smi(6)};
    auto direct = interp.run(*module, "add2", direct_args);
    VORTEX_EXPECT(direct.has_value());

    auto r = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::Int, ForeignArgKind::Int},
                 ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_shim), &env);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    NativeToGuestStub stub = std::move(*r);
    auto fn = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(
        stub.fn_ptr());
    VORTEX_EXPECT_EQ(fn(5, 6), 11u);
    VORTEX_EXPECT_EQ(stub.ctx->error, 0u);
    // The shim saw the canonical tagged words (the stub's re-box).
    VORTEX_EXPECT_EQ(env.seen_words[0], TaggedValue::smi(5).raw());
    VORTEX_EXPECT_EQ(env.seen_words[1], TaggedValue::smi(6).raw());
    // Parity: the engine's direct answer equals the stub-round-tripped one.
    VORTEX_EXPECT(direct.has_value() && direct->value.raw() ==
                                            TaggedValue::smi(11).raw());
}

// CEP:WHAT: Test: Bool/Null callback arguments arrive as canonical tagged words.
// CEP:WHY: DoD 4 (argument re-box rules).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_callback_bool_and_null_args) {
    ShimEnv env;  // force mode: the shim echoes a chosen word
    env.force = true;
    auto r = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::Bool, ForeignArgKind::Null},
                 ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_shim), &env);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    NativeToGuestStub stub = std::move(*r);
    auto fn = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t)>(
        stub.fn_ptr());
    (void)fn(1, 0);
    VORTEX_EXPECT_EQ(env.seen_words[0], TaggedValue::boolean(true).raw());
    VORTEX_EXPECT_EQ(env.seen_words[1], TaggedValue::null().raw());
    (void)fn(0, 0);
    VORTEX_EXPECT_EQ(env.seen_words[0], TaggedValue::boolean(false).raw());
}

// CEP:WHAT: Test: canonical bool words normalize to native 0/1; non-canonical words record the Type bit.
// CEP:WHY: DoD 7 (return normalization + named failure).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_callback_bool_return_normalization) {
    ShimEnv env;
    env.force = true;
    auto r = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::Null}, ForeignReturnKind::Bool),
        reinterpret_cast<void*>(&ffi_test_shim), &env);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    NativeToGuestStub stub = std::move(*r);
    auto fn = reinterpret_cast<uint64_t (*)(uint64_t)>(stub.fn_ptr());
    env.forced_return = TaggedValue::boolean(true).raw();
    VORTEX_EXPECT_EQ(fn(0), 1u);
    env.forced_return = TaggedValue::boolean(false).raw();
    VORTEX_EXPECT_EQ(fn(0), 0u);
    env.forced_return = TaggedValue::smi(3).raw();  // non-canonical word
    (void)fn(0);
    VORTEX_EXPECT_EQ(fn(0), 0u);
    VORTEX_EXPECT((stub.ctx->error & kFfiErrType) != 0);
}

// CEP:WHAT: Test: an out-of-int63 native argument records the Overflow bit with the null substitute.
// CEP:WHY: DoD 7 (named failure; documented substitute, never a silent wrap).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_callback_arg_overflow_records_named_bit) {
    ShimEnv env;
    env.force = true;
    auto r = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::Int}, ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_shim), &env);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    NativeToGuestStub stub = std::move(*r);
    auto fn = reinterpret_cast<uint64_t (*)(uint64_t)>(stub.fn_ptr());
    (void)fn(1ULL << 62);  // one past smi_max
    VORTEX_EXPECT((stub.ctx->error & kFfiErrOverflow) != 0);
    // The helper substituted the canonical null word (documented).
    VORTEX_EXPECT_EQ(env.seen_words[0], TaggedValue::null().raw());
}

// CEP:WHAT: Test: a non-Smi return word at an Int signature yields 0 and records the Type bit.
// CEP:WHY: DoD 7 (named failure).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_callback_return_type_error_yields_zero) {
    ShimEnv env;
    env.force = true;
    env.forced_return = TaggedValue::null().raw();  // non-Smi at Int return
    auto r = TrampolineGenerator::emit_native_to_guest(
        make_sig({}, ForeignReturnKind::Int),
        reinterpret_cast<void*>(&ffi_test_shim), &env);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;
    NativeToGuestStub stub = std::move(*r);
    auto fn = reinterpret_cast<uint64_t (*)()>(stub.fn_ptr());
    VORTEX_EXPECT_EQ(fn(), 0u);
    VORTEX_EXPECT((stub.ctx->error & kFfiErrType) != 0);
}

// ---- mixed walk (DoD 6) --------------------------------------------------------

// ---- exception bridge (Rule 132) -----------------------------------------------

// CEP:WHAT: Test: escaping C++ exceptions become named ForeignBoundary failures carrying the text.
// CEP:WHY: DoD 7 / Rule 132 (exceptions never cross into guest frames).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_exception_bridge_named_failures) {
    auto boom = ffi_boundary_call("test-boundary", []() -> int64_t {
        throw std::runtime_error("foreign detonation");
    });
    VORTEX_EXPECT(!boom.has_value());
    if (!boom) {
        VORTEX_EXPECT(boom.error().code == ErrorCode::ForeignBoundary);
        VORTEX_EXPECT(boom.error().message.find("test-boundary") !=
                      std::string::npos);
        VORTEX_EXPECT(boom.error().message.find("foreign detonation") !=
                      std::string::npos);
    }
    auto calm = ffi_boundary_call("test-boundary", []() -> int64_t { return 7; });
    VORTEX_EXPECT(calm.has_value() && *calm == 7);
    auto unknown = ffi_boundary_call("tb", []() -> int { throw 42; });
    VORTEX_EXPECT(!unknown.has_value());
    if (!unknown) {
        VORTEX_EXPECT(unknown.error().code == ErrorCode::ForeignBoundary);
        VORTEX_EXPECT(unknown.error().message.find("unknown foreign throw") !=
                      std::string::npos);
    }
}

// ---- emit refusals (named, side-effect free) ------------------------------------

// CEP:WHAT: Test: every documented refusal fires named with no registry side effects.
// CEP:WHY: DoD 8 (arity caps, F64/Ptr callback args, Null call-out args, non-SysV, null callee).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_emit_refusals) {
    const size_t before = ffi_trampoline_registry().count();
    // 7 integer-class args (stack-arg spill is the AAPCS64 batch).
    auto many_int = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Int, ForeignArgKind::Int,
                  ForeignArgKind::Int, ForeignArgKind::Int,
                  ForeignArgKind::Int, ForeignArgKind::Int,
                  ForeignArgKind::Int},
                 ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_nop), nullptr);
    VORTEX_EXPECT(!many_int.has_value());
    if (!many_int)
        VORTEX_EXPECT(many_int.error().code == ErrorCode::InvalidArgument);
    // 7 F64 args.
    auto many_f64 = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::F64, ForeignArgKind::F64,
                  ForeignArgKind::F64, ForeignArgKind::F64,
                  ForeignArgKind::F64, ForeignArgKind::F64,
                  ForeignArgKind::F64},
                 ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_nop), nullptr);
    VORTEX_EXPECT(!many_f64.has_value());
    // Null is call-out-refused (callback-direction kind).
    auto null_out = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Null}, ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_nop), nullptr);
    VORTEX_EXPECT(!null_out.has_value());
    // Non-SysV abi.
    ForeignSignature arm;
    arm.abi = AbiKind::Aapcs64;
    arm.args = {ForeignArgKind::Int};
    auto aapcs = TrampolineGenerator::emit_guest_to_native(
        arm, reinterpret_cast<void*>(&ffi_test_nop), nullptr);
    VORTEX_EXPECT(!aapcs.has_value());
    if (!aapcs) VORTEX_EXPECT(aapcs.error().code == ErrorCode::Unsupported);
    // Null foreign fn.
    auto null_fn = TrampolineGenerator::emit_guest_to_native(
        make_sig({ForeignArgKind::Int}, ForeignReturnKind::Void), nullptr,
        nullptr);
    VORTEX_EXPECT(!null_fn.has_value());
    // Callback-direction refusals: F64/Ptr args, arity cap.
    auto cb_f64 = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::F64}, ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_shim), nullptr);
    VORTEX_EXPECT(!cb_f64.has_value());
    auto cb_ptr = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::Ptr}, ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_shim), nullptr);
    VORTEX_EXPECT(!cb_ptr.has_value());
    auto cb_many = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::Int, ForeignArgKind::Int,
                  ForeignArgKind::Int, ForeignArgKind::Int,
                  ForeignArgKind::Int, ForeignArgKind::Int,
                  ForeignArgKind::Int},
                 ForeignReturnKind::Void),
        reinterpret_cast<void*>(&ffi_test_shim), nullptr);
    VORTEX_EXPECT(!cb_many.has_value());
    auto cb_null_shim = TrampolineGenerator::emit_native_to_guest(
        make_sig({ForeignArgKind::Int}, ForeignReturnKind::Void), nullptr,
        nullptr);
    VORTEX_EXPECT(!cb_null_shim.has_value());
    // No side effects: nothing was registered.
    VORTEX_EXPECT_EQ(ffi_trampoline_registry().count(), before);
}

// ---- registry pairing -------------------------------------------------------------

// CEP:WHAT: Test: register-on-emit / unregister-on-destroy pairing survives a move.
// CEP:WHY: Registry contract (stale ranges must never classify frames).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: VORTEX_EXPECT names the broken leg.
// CEP:ASSUMES: Module-level CEP:ASSUMES.
// CEP:COST: @cold.
// CEP:EVIDENCE: this test.
VORTEX_TEST(ffi_registry_pairs_with_stub_lifetime) {
    const size_t before = ffi_trampoline_registry().count();
    {
        auto r = TrampolineGenerator::emit_guest_to_native(
            make_sig({ForeignArgKind::Int}, ForeignReturnKind::Int),
            reinterpret_cast<void*>(&ffi_test_add), nullptr);
        VORTEX_EXPECT(r.has_value());
        if (!r) return;
        GuestToNativeStub stub = std::move(*r);
        VORTEX_EXPECT_EQ(ffi_trampoline_registry().count(), before + 1);
        const auto* range = ffi_trampoline_registry().lookup(
            reinterpret_cast<uintptr_t>(stub.memory.data()));
        VORTEX_EXPECT(range != nullptr && range->id == stub.id);
        // Move transfers the id; the moved-from wrapper owns nothing.
        GuestToNativeStub moved = std::move(stub);
        VORTEX_EXPECT(stub.id == 0);
        VORTEX_EXPECT(moved.id != 0);
        VORTEX_EXPECT(ffi_trampoline_registry().lookup(
                          reinterpret_cast<uintptr_t>(moved.memory.data()))
                          ->id == moved.id);
    }
    VORTEX_EXPECT_EQ(ffi_trampoline_registry().count(), before);
}
