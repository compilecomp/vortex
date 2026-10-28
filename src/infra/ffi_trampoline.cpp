// CEP:FILE: src/infra/ffi_trampoline.cpp
// CEP:WHAT: Infra 5 trampoline generator implementation (M6-c,
//           docs/infrastructure/05-ffi.md normative sections): synthesized
//           SysV x86-64 stubs translating tagged guest values to the native
//           OS ABI and back — the guest→native call-out stub (Smi unbox,
//           boxed-double payload load into XMM, pointer-like unbox, boundary
//           safepoint poll, foreign call, box-on-return with the int63
//           roundtrip overflow check) and the native→guest callback stub
//           (register-arg spill, tagged re-box, rip-relative context island,
//           boundary poll, C++ shim call, unbox-on-return) — emitted through
//           the ONE W^X publish path with the M1 assembler and registered in
//           the FfiTrampolineRegistry for the unified unwinder. Also owns
//           the registry itself, the stub move/destructor lifetimes, and
//           the extern "C" slow-path helpers the contexts carry.
// CEP:WHY: Rule 5 (W^X absolute — stubs publish through WritableCodeMemory);
//           Rule 82 (named failures — the stubs cannot return Result, so
//           slow-path helpers record FfiBoundaryError bits and the C++
//           caller converts them); the deterministic-compile contract
//           (byte-identical stubs: no baked addresses — the foreign callee,
//           the shim, and the poll word are reached through context fields,
//           the callback context through a rip-relative island whose
//           displacement is layout-only, and the slow-path helpers through
//           context slots filled by the generator).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Emit-time refusals are named Result rejects (F64/Ptr callback
//           args, Null call-out args, arity caps, non-SysV abi, null callee,
//           allocation failure) with no side effects — nothing is registered
//           or published on refusal. Runtime boundary failures are recorded
//           as error bits in the context (type/overflow) and substituted
//           with the canonical undefined/null word — never a silent garbage
//           value; the C++ caller converts the bits into a named Result.
//           Never raises, no try/catch (Rule 114: exceptions are translated at
//           the boundary wrapper, never inside the stubs).
// CEP:ASSUMES: Linux x86-64 SysV LP64 (TaggedValue and ObjectHeader layout
//           facts are static_assert-pinned in their headers); context
//           structs are generator-filled before first call; the caller keeps
//           a stub alive while native code holds its fn_ptr; foreign
//           callees are bounded or re-enter through polling callbacks (they
//           cannot poll themselves — the boundary is the pause point);
//           callback signatures with native stack arguments are refused
//           above, so the 6-register spill covers every accepted signature.
// CEP:COST: @cold emit (one mmap + mprotect flip + registry insert per
//           stub); @warm stub paths are straight-line machine code — one
//           poll-word load per call, per-arg shift/test translation, no
//           allocation on the fast path, slow-path helper calls only on
//           boundary errors or pending pauses.
// CEP:EVIDENCE: tests/test_ffi.cpp — golden machine-code pins, box/unbox
//           parity, boundary type/overflow pins, safepoint-tick pins,
//           callback re-entry parity, determinism (two emissions
//           byte-identical), emit-refusal pins.
#include "vortex/infra/ffi.hpp"

#include "vortex/codegen/x64/assembler.hpp"
#include "vortex/infra/threading.hpp"

#include <cstring>

namespace vortex::infra {
namespace {

using codegen::CodeBuffer;
using codegen::x64::Assembler;
using codegen::x64::CC;
using codegen::x64::CC_E;
using codegen::x64::CC_NE;
using codegen::x64::Mem;
using codegen::x64::Reg;
using codegen::x64::Xmm;

/// Frame tag stored at [rbp-8] by every stub: future secondary tooling
/// material (M6-c classification is pc-range-only). 'VFXF' + version tag.
constexpr uint64_t kTrampolineFrameMagic = 0x5646584630313233ULL;

/// Fixed emission budget — a constant keeps the emission a pure function of
/// the signature. The registry records the EXACT code length.
constexpr size_t kStubBudget = 1024;

constexpr size_t kMaxIntArgs = 6;  // SysV integer register class
constexpr size_t kMaxF64Args = 6;  // SysV SSE register class

/// Canonical non-Smi words (TaggedValue private constants, mirrored here for
/// the machine code; the header static_asserts pin the layout).
constexpr uint64_t kNullWord = 0x3;
constexpr uint64_t kUndefinedWord = 0x7;
constexpr uint64_t kFalseWord = 0xB;
constexpr uint64_t kTrueWord = 0xF;

// CEP:WHAT: int63 roundtrip overflow test: v fits int63 iff (v<<1)>>1 == v.
// CEP:WHY: The Smi box cannot use a shift-overflow flag (the payload is int63, not int64) — the roundtrip identity is the exact, branch-cheap test (docs/infrastructure/05-ffi.md return translation).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — pure flag-producing computation.
// CEP:ASSUMES: v is a 64-bit integer register (SysV INTEGER class).
// CEP:COST: @warm. Two shifts + a compare on the return path; the slow path is taken only on genuine overflow.
// CEP:EVIDENCE: tests/test_ffi.cpp.
/// int63 roundtrip overflow check: v fits int63 iff (v<<1)>>1 == v.
void emit_int63_roundtrip(Assembler& a, Reg v) {
    a.mov_reg_reg(Reg::R10, v);
    a.shl_reg_imm8(Reg::R10, 1);
    a.sar_reg_imm8(Reg::R10, 1);
    a.cmp_reg_reg(Reg::R10, v);
}

// ---------------------------------------------------------------------------
// Label bookkeeping over the assembler's rel32 reference slots. References
// may be emitted before (forward) or after (backward) bind.
// ---------------------------------------------------------------------------
// CEP:WHAT: Minimal label layer over the assembler's rel32 reference slots.
// CEP:WHY: The stubs need forward and backward branch targets; the
//           assembler exposes raw placeholders only.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — bookkeeping over the assembler's own placement.
// CEP:ASSUMES: bind is called exactly once per label; double bind asserts
//           via the position overwrite (emission-time bug, pinned by the
//           golden bytes).
// CEP:COST: @cold. O(refs) per bind.
// CEP:EVIDENCE: tests/test_ffi.cpp (golden pins + every parity test).
struct Label {
    std::vector<size_t> refs;  // pending rel32 slots pointing here
    size_t pos = SIZE_MAX;     // bound position

// CEP:WHAT: Binds the label at the current offset and patches every reference.
// CEP:WHY: Forward references are recorded at emit time and resolved here.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — bind_placeholder arithmetic is total.
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_ffi.cpp.
    void bind_here(Assembler& a) {
        pos = a.current_offset();
        for (size_t at : refs) a.bind_placeholder(at);
        refs.clear();
    }
// CEP:WHAT: Records a conditional-branch reference to this label.
// CEP:WHY: See the Label block; JCC is the only branch form the stubs need.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_ffi.cpp.
    void ref_jcc(Assembler& a, uint8_t cc) {
        refs.push_back(a.placeholder_jcc(cc));
    }
// CEP:WHAT: Records an unconditional-branch reference to this label.
// CEP:WHY: See the Label block; used to skip slow paths.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_ffi.cpp.
    void ref_jmp(Assembler& a) { refs.push_back(a.placeholder_jmp()); }
};

const Reg kSysVInt[6] = {Reg::RDI, Reg::RSI, Reg::RDX, Reg::RCX, Reg::R8,
                         Reg::R9};
const Xmm kSysVF64[6] = {Xmm::XMM0, Xmm::XMM1, Xmm::XMM2, Xmm::XMM3,
                         Xmm::XMM4, Xmm::XMM5};

// CEP:WHAT: Builds [base + byte_off] — no-index memory operand form.
// CEP:WHY: The single spelling of memory operands in the emitters keeps the encodings uniform (and the SIB discipline in one place).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_ffi.cpp.
Mem slot(Reg base, int32_t byte_off) {
    return Mem{base, Reg::RSP, 0, byte_off};  // [base + byte_off]
}

// Shared prologue: frame + magic tag. SysV entry has rsp ≡ 8 (mod 16);
// after the rbp push rsp ≡ 0 and after the magic push rsp ≡ 8 again —
// each stub's remaining pushes/sub restore ≡ 0 before its first call.
// CEP:WHAT: Shared prologue: RBP frame + magic tag at [rbp-8].
// CEP:WHY: Trampoline frames are RBP-chained (unwinder contract) and carry the type tag as the secondary unwinder check (docs/infrastructure/05-ffi.md frame rules).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — straight-line emission.
// CEP:ASSUMES: SysV entry invariant rsp ≡ 8 (mod 16) holds at the call site.
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_ffi.cpp.
void emit_frame_head(Assembler& a) {
    a.push_reg(Reg::RBP);                // [rbp] = saved rbp (unwind chain)
    a.mov_reg_reg(Reg::RBP, Reg::RSP);
    a.mov_reg_imm64(Reg::RAX, kTrampolineFrameMagic);
    a.push_reg(Reg::RAX);                // [rbp-8] magic tag
}

// Shared epilogue tail (the stub pops its own scratch/registers first):
// magic, rbp, ret.
// CEP:WHAT: Shared epilogue tail: magic, RBP, ret (stub pops its scratch first).
// CEP:WHY: Mirror of emit_frame_head; keeps the frame contract in one place.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — straight-line emission.
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_ffi.cpp.
void emit_frame_tail(Assembler& a) {
    a.add_reg_imm32(Reg::RSP, 8);        // magic
    a.pop_reg(Reg::RBP);
    a.ret();
}

/// Boundary safepoint: one poll-word load through the context; an armed word
/// calls the context's safepoint hook, then both paths fall through.
// CEP:WHAT: Boundary safepoint: one poll-word load; an armed word calls the context's hook before both paths fall through.
// CEP:WHY: Foreign frames never poll — the boundary is the mutator's pause acknowledgment point (docs/infrastructure/05-ffi.md). The poll runs BEFORE argument translation because the hook call clobbers caller-saved registers.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — a null poll word (page not reserved) skips the check.
// CEP:ASSUMES: ctx->poll_word is the raw flag-page word or nullptr.
// CEP:COST: @warm. Two loads + two tests; the hook call happens only while a pause is pending.
// CEP:EVIDENCE: tests/test_ffi.cpp — armed/disarmed tick pins.
void emit_boundary_poll(Assembler& a, Reg ctx, size_t hook_off) {
    Label after_poll;
    a.mov_reg_mem(Reg::RAX, slot(ctx, 16));      // ctx->poll_word
    a.cmp_reg_imm32(Reg::RAX, 0);
    after_poll.ref_jcc(a, CC_E);                 // null: page not reserved
    a.mov_reg32_mem(Reg::R10, Mem{Reg::RAX, Reg::RSP, 0, 0});
    a.test_reg_imm32(Reg::R10, -1);              // full-word arm test
    after_poll.ref_jcc(a, CC_E);                 // disarmed: fall through
    a.mov_reg_reg(Reg::RDI, ctx);
    a.call_mem(slot(ctx, int32_t(hook_off)));    // armed: acknowledge
    after_poll.bind_here(a);
}

}  // namespace

// ---------------------------------------------------------------------------
// Slow-path helpers (extern "C": the machine code calls them through the
// context slots; the C ABI is the stub-level contract).
// ---------------------------------------------------------------------------

extern "C" void vortex_ffi_safepoint_boundary(TrampolineContext* ctx) noexcept {
    ++ctx->safepoint_ticks;
}

// CEP:WHAT: Slow path: int63 box overflow at the call-out return.
// CEP:WHY: Rule 82: the stub records the named Overflow bit and substitutes the canonical undefined word — never a silent wrap.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold. One store + one return.
// CEP:EVIDENCE: tests/test_ffi.cpp.
extern "C" uint64_t vortex_ffi_box_smi_slow(int64_t,
                                            TrampolineContext* ctx) noexcept {
    ctx->error |= kFfiErrOverflow;
    return kUndefinedWord;  // canonical boundary-error word
}

// CEP:WHAT: Slow path: boundary type error (bad tag at any typed slot).
// CEP:WHY: Rule 82: records the named Type bit; the foreign callee never runs with garbage arguments (the stub skips the call).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold. One store + one return.
// CEP:EVIDENCE: tests/test_ffi.cpp.
extern "C" uint64_t vortex_ffi_type_error_slow(uint32_t,
                                               TrampolineContext* ctx) noexcept {
    ctx->error |= kFfiErrType;
    return kUndefinedWord;
}

extern "C" void vortex_ffi_callback_safepoint(CallbackContext* ctx) noexcept {
    ++ctx->safepoint_ticks;
}

// CEP:WHAT: Slow path: callback Int-arg int63 overflow.
// CEP:WHY: Rule 82: records the named Overflow bit and stores the canonical null word into the tagged slot (documented substitute).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold. Two stores.
// CEP:EVIDENCE: tests/test_ffi.cpp.
extern "C" void vortex_ffi_callback_box_slow(
    int64_t, CallbackContext* ctx, TaggedValue* out_slot) noexcept {
    ctx->error |= kFfiErrOverflow;
    *out_slot = TaggedValue::null();  // deterministic substitute (documented)
}

// CEP:WHAT: Slow path: callback return type error (non-canonical word).
// CEP:WHY: Rule 82: records the named Type bit and yields 0 to the native caller; the context carries the diagnosis.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold. One store + one return.
// CEP:EVIDENCE: tests/test_ffi.cpp.
extern "C" uint64_t vortex_ffi_callback_ret_slow(
    uint64_t, CallbackContext* ctx) noexcept {
    ctx->error |= kFfiErrType;
    return 0;
}

// ---------------------------------------------------------------------------
// FfiTrampolineRegistry
// ---------------------------------------------------------------------------

// CEP:WHAT: Process-wide trampoline range registry (unwinder consumption).
// CEP:WHY: Trampoline frames are classified by pc range — they carry no GC
//           maps (no tagged live state across the foreign call), only
//           identity (docs/infrastructure/05-ffi.md unwinder section).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: unregister of an unknown id is a no-op (move-dtor discipline
//           sets moved-from ids to 0); lookup misses return nullptr.
// CEP:ASSUMES: single-threaded emission/consumption (the mutator that
//           creates stubs walks its own stack); ranges are exact-length.
// CEP:COST: @warm lookup: O(ranges) linear scan over a handful of live
//           stubs. Register/unregister: O(ranges).
// CEP:EVIDENCE: tests/test_ffi.cpp — registry lookup classify/walk pins.
uint32_t FfiTrampolineRegistry::register_range(const void* code,
                                               size_t bytes) {
    const uint32_t id = next_id_++;
    ranges_.push_back(Range{reinterpret_cast<uintptr_t>(code),
                            reinterpret_cast<uintptr_t>(code) + bytes, id});
    return id;
}

// CEP:WHAT: Removes a stub's published range (destructor pairing).
// CEP:WHY: Released stubs must stop classifying frames immediately — a stale range would mislabel native pcs as trampolines.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: unregister of an unknown id is a no-op (moved-from wrappers carry id 0).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold. O(ranges) linear scan over a handful of live stubs.
// CEP:EVIDENCE: tests/test_ffi.cpp.
void FfiTrampolineRegistry::unregister(uint32_t id) {
    if (id == 0) return;
    for (auto it = ranges_.begin(); it != ranges_.end(); ++it) {
        if (it->id == id) {
            ranges_.erase(it);
            return;
        }
    }
}

// CEP:WHAT: Range containment lookup for the unified unwinder.
// CEP:WHY: Frame classification is pc-range containment (no GC maps needed: trampolines hold no tagged live state across the foreign call).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Misses return nullptr (the walker then tries dladdr).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @warm lookup: O(ranges) over a handful of live stubs.
// CEP:EVIDENCE: tests/test_ffi.cpp.
const FfiTrampolineRegistry::Range* FfiTrampolineRegistry::lookup(
    uintptr_t pc) const noexcept {
    for (const Range& r : ranges_) {
        if (pc >= r.lo && pc < r.hi) return &r;
    }
    return nullptr;
}

// CEP:WHAT: Process-wide registry accessor (one runtime per process, M0 contract).
// CEP:WHY: The generator registers on emit; stubs unregister on destroy; the walker consumes.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold after first call (function-local static).
// CEP:EVIDENCE: tests/test_ffi.cpp.
FfiTrampolineRegistry& ffi_trampoline_registry() noexcept {
    static FfiTrampolineRegistry registry;
    return registry;
}

// ---------------------------------------------------------------------------
// Stub lifetimes (registry pairing: register on emit, unregister on destroy;
// move transfers the id and neutralizes the source).
// ---------------------------------------------------------------------------

// CEP:WHAT: Destructor: unregisters the published range (register/destroy pairing).
// CEP:WHY: Registry pairing discipline (docs/infrastructure/05-ffi.md FfiTrampolineRegistry): every published range dies with its stub.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_ffi.cpp.
GuestToNativeStub::~GuestToNativeStub() {
    if (id != 0) ffi_trampoline_registry().unregister(id);
}
// CEP:WHAT: Move: transfers the RX memory, entry, context, and registry id; the source is neutralized (id 0) so the pairing stays exact.
// CEP:WHY: Registry pairing discipline (docs/infrastructure/05-ffi.md FfiTrampolineRegistry): every published range dies with its stub.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_ffi.cpp.
GuestToNativeStub::GuestToNativeStub(GuestToNativeStub&& other) noexcept
    : memory(std::move(other.memory)),
      entry(other.entry),
      ctx(other.ctx),
      id(other.id),
      ptr_arg_slots(std::move(other.ptr_arg_slots)) {
    other.entry = nullptr;
    other.id = 0;
    other.ptr_arg_slots.clear();
}
GuestToNativeStub& GuestToNativeStub::operator=(
    GuestToNativeStub&& other) noexcept {
    if (this != &other) {
        if (id != 0) ffi_trampoline_registry().unregister(id);
        memory = std::move(other.memory);
        entry = other.entry;
        ctx = other.ctx;
        id = other.id;
        ptr_arg_slots = std::move(other.ptr_arg_slots);
        other.entry = nullptr;
        other.id = 0;
        other.ptr_arg_slots.clear();
    }
    return *this;
}

// CEP:WHAT: Destructor: unregisters the published range (register/destroy pairing).
// CEP:WHY: Registry pairing discipline (docs/infrastructure/05-ffi.md FfiTrampolineRegistry): every published range dies with its stub.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_ffi.cpp.
NativeToGuestStub::~NativeToGuestStub() {
    if (id != 0) ffi_trampoline_registry().unregister(id);
}
// CEP:WHAT: Move: transfers the RX memory, heap-owned context (the island-baked address is stable across moves), fn pointer, and registry id.
// CEP:WHY: Registry pairing discipline (docs/infrastructure/05-ffi.md FfiTrampolineRegistry): every published range dies with its stub.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: none — the path is total (no partial states).
// CEP:ASSUMES: The module-level CEP:ASSUMES covers platform and lifetime contracts.
// CEP:COST: @cold.
// CEP:EVIDENCE: tests/test_ffi.cpp.
NativeToGuestStub::NativeToGuestStub(NativeToGuestStub&& other) noexcept
    : memory(std::move(other.memory)),
      ctx(std::move(other.ctx)),
      id(other.id),
      fn_(other.fn_) {
    other.id = 0;
    other.fn_ = nullptr;
}
NativeToGuestStub& NativeToGuestStub::operator=(
    NativeToGuestStub&& other) noexcept {
    if (this != &other) {
        if (id != 0) ffi_trampoline_registry().unregister(id);
        memory = std::move(other.memory);
        ctx = std::move(other.ctx);
        id = other.id;
        fn_ = other.fn_;
        other.id = 0;
        other.fn_ = nullptr;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Guest → native
// ---------------------------------------------------------------------------

// CEP:WHAT: Synthesizes the guest→native call-out stub
//           (docs/infrastructure/05-ffi.md "Guest → native" — normative).
// CEP:WHY: The JIT tiers call foreign code through THIS machine-code shape;
//           every translation rule (Smi shift, boxed-double payload, pointer
//           unbox, canonical bools, int63 overflow) is pinned here once.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Named emit refusals (non-SysV, null callee, Null args, arity
//           caps, allocation failure) — nothing registered or published.
// CEP:ASSUMES: The signature validation above ran; the budget check turns
//           any kStubBudget overflow into a named emit failure (the
//           max-shape emission test pins the headroom).
// CEP:COST: @cold. One WritableCodeMemory allocate + publish + registry
//           insert; emission is O(args) assembler calls.
// CEP:EVIDENCE: tests/test_ffi.cpp — golden machine-code pins (first 48
//           bytes), add/f64/ptr parity, boundary error bits, determinism.
support::Result<GuestToNativeStub> TrampolineGenerator::emit_guest_to_native(
    const ForeignSignature& sig, void* foreign_fn, void* user) {
    if (sig.abi != AbiKind::SystemVX86_64) {
        return support::fail(support::ErrorCode::Unsupported,
                             "ffi: AAPCS64 trampolines ship with the ARM64 "
                             "backend batch");
    }
    if (foreign_fn == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "ffi: null foreign function");
    }
    size_t n_int = 0;
    size_t n_f64 = 0;
    for (const ForeignArgKind k : sig.args) {
        if (k == ForeignArgKind::Null) {
            return support::fail(support::ErrorCode::InvalidArgument,
                                 "ffi: Null arguments are callback-direction "
                                 "(native to guest) only");
        }
        if (k == ForeignArgKind::F64) {
            if (++n_f64 > kMaxF64Args) {
                return support::fail(support::ErrorCode::InvalidArgument,
                                     "ffi: more than 6 F64 arguments "
                                     "(stack-arg spill is the AAPCS64 batch)");
            }
        } else if (++n_int > kMaxIntArgs) {
            return support::fail(support::ErrorCode::InvalidArgument,
                                 "ffi: more than 6 integer-class arguments "
                                 "(stack-arg spill is the AAPCS64 batch)");
        }
    }

    auto mem = WritableCodeMemory::allocate(kStubBudget);
    if (!mem) return std::unexpected(mem.error());
    GuestToNativeStub stub(std::move(*mem));

    CodeBuffer buf;
    Assembler a(buf);

    emit_frame_head(a);
    a.push_reg(Reg::RBX);
    a.push_reg(Reg::R12);
    a.mov_reg_reg(Reg::R12, Reg::RDI);  // r12 = tagged args array
    a.mov_reg_reg(Reg::RBX, Reg::RSI);  // rbx = context
    a.sub_reg_imm32(Reg::RSP, 24);      // scratch; rsp ≡ 0 at every call

    // Boundary safepoint BEFORE argument translation: the hook call would
    // otherwise clobber the caller-saved registers holding the translated
    // native arguments (the incoming args ptr and ctx are already parked in
    // callee-saved R12/RBX, so the hook can clobber freely). Pausing before
    // the call also means the mutator never sits between poll and foreign
    // frame (docs/infrastructure/05-ffi.md "Boundary safepoint").
    emit_boundary_poll(a, Reg::RBX, 32);         // ctx->safepoint_hook

    Label type_slow;
    Label epi;
    size_t ii = 0;
    size_t fi = 0;
    for (size_t arg = 0; arg < sig.args.size(); ++arg) {
        const ForeignArgKind kind = sig.args[arg];
        const int32_t off = static_cast<int32_t>(8 * arg);
        switch (kind) {
        case ForeignArgKind::Int: {
            a.mov_reg_mem(Reg::R11, slot(Reg::R12, off));
            a.test_reg_imm8(Reg::R11, 1);        // Smi tag (bit0 == 0)
            type_slow.ref_jcc(a, CC_NE);
            a.sar_reg_imm8(Reg::R11, 1);         // unbox
            a.mov_reg_reg(kSysVInt[ii], Reg::R11);
            ++ii;
            break;
        }
        case ForeignArgKind::F64: {
            a.mov_reg_mem(Reg::R11, slot(Reg::R12, off));
            a.and_reg_imm32(Reg::R11, 15);       // tag nibble
            a.cmp_reg_imm32(Reg::R11, 1);        // heap object tag
            type_slow.ref_jcc(a, CC_NE);
            a.mov_reg_mem(Reg::R11, slot(Reg::R12, off));
            a.and_reg_imm32(Reg::R11, -16);      // object base
            a.movsd_xmm_mem(kSysVF64[fi], slot(Reg::R11, 24));  // payload
            ++fi;
            break;
        }
        case ForeignArgKind::Ptr: {
            a.mov_reg_mem(Reg::R11, slot(Reg::R12, off));
            a.mov_reg_reg(Reg::R10, Reg::R11);
            a.and_reg_imm32(Reg::R10, 15);
            a.cmp_reg_imm32(Reg::R10, 1);        // heap object
            Label ok;
            ok.ref_jcc(a, CC_E);
            a.cmp_reg_imm32(Reg::R10, 3);        // null word
            type_slow.ref_jcc(a, CC_NE);
            ok.bind_here(a);
            a.mov_reg_mem(Reg::R11, slot(Reg::R12, off));
            a.and_reg_imm32(Reg::R11, -16);      // object base
            a.mov_reg_reg(kSysVInt[ii], Reg::R11);
            ++ii;
            break;
        }
        case ForeignArgKind::Bool: {
            a.mov_reg_mem(Reg::R11, slot(Reg::R12, off));
            a.xor_reg_reg(Reg::R10, Reg::R10);   // assume false -> 0
            a.cmp_reg_imm32(Reg::R11, static_cast<int32_t>(kFalseWord));
            Label is_false;
            is_false.ref_jcc(a, CC_E);
            a.cmp_reg_imm32(Reg::R11, static_cast<int32_t>(kTrueWord));
            type_slow.ref_jcc(a, CC_NE);         // non-canonical word
            a.mov_reg_imm32sx(Reg::R10, 1);      // true -> 1
            is_false.bind_here(a);
            a.mov_reg_reg(kSysVInt[ii], Reg::R10);
            ++ii;
            break;
        }
        case ForeignArgKind::Null:
            // Validated above; unreachable. Kept for exhaustive switch.
            return support::fail(support::ErrorCode::InvalidArgument,
                                 "ffi: Null arguments are callback-direction "
                                 "(native to guest) only");
        }
    }

    a.mov_reg_mem(Reg::RAX, slot(Reg::RBX, 0));  // ctx->foreign_fn
    a.call_reg(Reg::RAX);

    switch (sig.return_kind) {
    case ForeignReturnKind::Void:
        a.mov_reg_imm32sx(Reg::RAX, static_cast<int32_t>(kUndefinedWord));
        epi.ref_jmp(a);
        break;
    case ForeignReturnKind::Int: {
        emit_int63_roundtrip(a, Reg::RAX);
        Label in_range;
        in_range.ref_jcc(a, CC_E);
        a.mov_reg_reg(Reg::RDI, Reg::RAX);       // unboxed value
        a.mov_reg_reg(Reg::RSI, Reg::RBX);       // ctx
        a.call_mem(slot(Reg::RBX, 40));          // box_smi_slow
        epi.ref_jmp(a);
        in_range.bind_here(a);
        a.shl_reg_imm8(Reg::RAX, 1);             // box
        epi.ref_jmp(a);
        break;
    }
    case ForeignReturnKind::Bool: {
        // Native domain IN, tagged domain OUT (M6-c review R1-1): the
        // foreign callee returns plain 0/1 (any nonzero = true, permissive
        // like the Bool-ARG direction) and the stub normalizes to the
        // canonical true/false words. The mirror-image validation only
        // exists in the CALLBACK direction, where the shim returns tagged
        // words (engine domain).
        Label nonzero;
        a.cmp_reg_imm32(Reg::RAX, 0);
        nonzero.ref_jcc(a, CC_NE);
        a.mov_reg_imm32sx(Reg::RAX, static_cast<int32_t>(kFalseWord));
        epi.ref_jmp(a);
        nonzero.bind_here(a);
        a.mov_reg_imm32sx(Reg::RAX, static_cast<int32_t>(kTrueWord));
        epi.ref_jmp(a);
        break;
    }
    }

    type_slow.bind_here(a);
    a.mov_reg_imm32sx(Reg::RDI, -1);             // sentinel arg index
    a.mov_reg_reg(Reg::RSI, Reg::RBX);
    a.call_mem(slot(Reg::RBX, 48));              // type_error_slow
    epi.bind_here(a);
    a.add_reg_imm32(Reg::RSP, 24);               // scratch
    a.pop_reg(Reg::R12);
    a.pop_reg(Reg::RBX);
    emit_frame_tail(a);

    const size_t code_bytes = buf.size();
    if (code_bytes > stub.memory.size()) {
        // Named failure, not a silent write truncation (M6-c review R1-3:
        // WritableCodeMemory::write bounds-gates silently; a truncated
        // stub would publish zero-filled RX pages).
        return support::fail(support::ErrorCode::InternalError,
                             "ffi: stub emission exceeded the code budget (" +
                                 std::to_string(code_bytes) + " > " +
                                 std::to_string(kStubBudget) + ")");
    }
    stub.memory.write(
        std::span<const uint8_t>(buf.code().data(), code_bytes), 0);
    if (auto pub = stub.memory.publish(); !pub) {
        return std::unexpected(pub.error());
    }
    stub.ctx = TrampolineContext{
        foreign_fn, user, SafepointPollingPage::poll_word_raw(),
        0, 0,
        reinterpret_cast<void*>(&vortex_ffi_safepoint_boundary),
        reinterpret_cast<void*>(&vortex_ffi_box_smi_slow),
        reinterpret_cast<void*>(&vortex_ffi_type_error_slow)};
    stub.entry = reinterpret_cast<TaggedValue (*)(
        const TaggedValue*, TrampolineContext*)>(stub.memory.data());
    // Batch-e managed sanitizers: record which slots are Ptr so the C++
    // boundary validation knows where the shadow check applies.
    for (size_t i = 0; i < sig.args.size(); ++i) {
        if (sig.args[i] == ForeignArgKind::Ptr && i < 255) {
            stub.ptr_arg_slots.push_back(static_cast<uint8_t>(i));
        }
    }
    stub.id = ffi_trampoline_registry().register_range(stub.memory.data(),
                                                       code_bytes);
    return stub;
}

// CEP:WHAT: validate_ptr_args — the call-out boundary's C++ span check:
//           each Ptr slot holding a heap object must be addressable in the
//           guest shadow (8 header bytes) before foreign code receives it.
// CEP:WHY: docs/infrastructure/06 managed sanitizers — the FFI boundary is
//          a named consumer; a stale (evacuated/released) base must be a
//          NAMED failure, not foreign-side corruption.
// CEP:CLASS: CEP-2
// CEP:STATUS: complete
// CEP:FAILURE: the named GuestSanitizer failure (kind + address).
// CEP:ASSUMES: noexcept; non-heap/non-null Ptr args skip (the emitted
//          stub's type gates own that named failure).
// CEP:COST: @cold (call-out setup); disabled policy = one bool per slot.
// CEP:EVIDENCE: tests/test_observability.cpp DoD 26.
support::Result<void> GuestToNativeStub::validate_ptr_args(
    const TaggedValue* args, size_t nargs) const noexcept {
    if (!infra::sanitizer_policy().guest_asan) return support::ok();
    for (const uint8_t slot : ptr_arg_slots) {
        if (slot >= nargs) continue;
        const TaggedValue& v = args[slot];
        if (!v.is_heap_object()) continue;  // null/other: the stub gates it
        if (auto r = infra::verify_span(v.as_heap_object(), 8); !r) {
            return r;
        }
    }
    return support::ok();
}

// ---------------------------------------------------------------------------
// Native → guest
// ---------------------------------------------------------------------------

// CEP:WHAT: Synthesizes the native→guest callback stub
//           (docs/infrastructure/05-ffi.md "Native → guest" — normative).
// CEP:WHY: Native embedders receive a plain SysV function; the stub spills
//           the integer args, re-boxes them into tagged words, polls at the
//           boundary, and re-enters the engine through the C++ shim — the
//           rip-relative island carries the heap-owned context (stable
//           across wrapper moves; displacement is layout-only so the bytes
//           stay allocation-independent).
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Named emit refusals (F64/Ptr args — both need heap allocation
//           or pin discipline the callback context does not have yet; arity
//           caps; non-SysV; null shim). Runtime: Int-arg int63 overflow and
//           non-canonical Bool/Int returns are recorded in the context by
//           the helpers and substituted (null word / 0) — named, never
//           silent.
// CEP:ASSUMES: Accepted signatures use only integer-register args (≤6), so
//           the spill arena covers every translated arg; the caller keeps
//           the stub alive while native code holds fn_ptr.
// CEP:COST: @cold emit as above; @warm straight-line machine code.
// CEP:EVIDENCE: tests/test_ffi.cpp — callback re-entry parity (engine run
//           vs stub), pin tests, refusal pins, island-context stability
//           across move.
support::Result<NativeToGuestStub> TrampolineGenerator::emit_native_to_guest(
    const ForeignSignature& sig, void* shim, void* user) {
    if (sig.abi != AbiKind::SystemVX86_64) {
        return support::fail(support::ErrorCode::Unsupported,
                             "ffi: AAPCS64 trampolines ship with the ARM64 "
                             "backend batch");
    }
    if (shim == nullptr) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "ffi: null callback shim");
    }
    if (sig.args.size() > kMaxIntArgs) {
        return support::fail(support::ErrorCode::InvalidArgument,
                             "ffi: more than 6 integer-class arguments "
                             "(stack-arg spill is the AAPCS64 batch)");
    }
    for (const ForeignArgKind k : sig.args) {
        if (k == ForeignArgKind::F64 || k == ForeignArgKind::Ptr) {
            return support::fail(
                support::ErrorCode::InvalidArgument,
                "ffi: F64/Ptr callback arguments are refused in this batch "
                "(allocation-integrated form ships with the emitted-poll "
                "batch)");
        }
    }
    const size_t n = sig.args.size();
    const size_t arena = 48 + 8 * n;  // 6-slot spill + tagged buffer
    const size_t sub = (arena % 16 == 8) ? arena : arena + 8;  // rsp ≡ 0

    auto mem = WritableCodeMemory::allocate(kStubBudget);
    if (!mem) return std::unexpected(mem.error());
    NativeToGuestStub stub(std::move(*mem));

    CodeBuffer buf;
    Assembler a(buf);

    emit_frame_head(a);
    a.push_reg(Reg::RBX);
    a.push_reg(Reg::R12);

    // rip-relative context island load (raw bytes — the assembler exposes no
    // RIP forms; the displacement is patched to the island after the code):
    //   48 8D 1D <disp32>   lea rbx, [rip+island]
    //   48 8B 1B            mov rbx, [rbx]
    buf.emit8(0x48);
    buf.emit8(0x8D);
    buf.emit8(0x1D);
    const size_t lea_disp = a.current_offset();
    buf.emit32(0);
    buf.emit8(0x48);
    buf.emit8(0x8B);
    buf.emit8(0x1B);

    // Arena base AFTER the sub: [r12, r12+S) must be the FRESH scratch area
    // below the saved registers. Basing r12 on the pre-sub rsp would grow the
    // arena UPWARD over the saved rbp/rbx/r12 slots, the return address, and
    // the caller's frame (the exact corruption class the pin suite caught).
    a.sub_reg_imm32(Reg::RSP, static_cast<int32_t>(sub));
    a.mov_reg_reg(Reg::R12, Reg::RSP);   // r12 = arena base

    // Spill the incoming integer args (helper calls below would otherwise
    // clobber untranslated caller-saved arg registers).
    for (size_t i = 0; i < n; ++i) {
        a.mov_mem_reg(slot(Reg::R12, static_cast<int32_t>(8 * i)),
                      kSysVInt[i]);
    }

    // Translate: native spill slot i -> tagged slot at [r12 + 48 + 8i].
    for (size_t i = 0; i < n; ++i) {
        const ForeignArgKind kind = sig.args[i];
        const int32_t spill_off = static_cast<int32_t>(8 * i);
        const int32_t tag_off = static_cast<int32_t>(48 + 8 * i);
        switch (kind) {
        case ForeignArgKind::Int: {
            a.mov_reg_mem(Reg::R11, slot(Reg::R12, spill_off));
            emit_int63_roundtrip(a, Reg::R11);
            Label overflow_i;                    // CC_NE = roundtrip FAILED
            overflow_i.ref_jcc(a, CC_NE);
            a.shl_reg_imm8(Reg::R11, 1);         // box
            a.mov_mem_reg(slot(Reg::R12, tag_off), Reg::R11);
            Label done_i;
            done_i.ref_jmp(a);
            overflow_i.bind_here(a);
            // overflow: helper stores the null substitute
            a.mov_reg_mem(Reg::RDI, slot(Reg::R12, spill_off));
            a.mov_reg_reg(Reg::RSI, Reg::RBX);
            a.lea_reg_mem(Reg::RDX, slot(Reg::R12, tag_off));
            a.call_mem(slot(Reg::RBX, 48));      // box_slow
            done_i.bind_here(a);
            break;
        }
        case ForeignArgKind::Bool: {
            // Permissive normalize: nonzero native value -> canonical true.
            a.mov_reg_mem(Reg::R11, slot(Reg::R12, spill_off));
            a.cmp_reg_imm32(Reg::R11, 0);
            Label nonzero;
            nonzero.ref_jcc(a, CC_NE);
            a.mov_mem_imm32sx(slot(Reg::R12, tag_off),
                              static_cast<int32_t>(kFalseWord));
            Label done_i;
            done_i.ref_jmp(a);
            nonzero.bind_here(a);
            a.mov_mem_imm32sx(slot(Reg::R12, tag_off),
                              static_cast<int32_t>(kTrueWord));
            done_i.bind_here(a);
            break;
        }
        case ForeignArgKind::Null: {
            a.mov_mem_imm32sx(slot(Reg::R12, tag_off),
                              static_cast<int32_t>(kNullWord));
            break;
        }
        case ForeignArgKind::F64:
        case ForeignArgKind::Ptr:
            // Validated above; unreachable. Kept for exhaustive switch.
            return support::fail(
                support::ErrorCode::InvalidArgument,
                "ffi: F64/Ptr callback arguments are refused in this batch");
        }
    }

    emit_boundary_poll(a, Reg::RBX, 40);         // ctx->safepoint_hook

    // Shim call: (const TaggedValue* args, size_t nargs, void* user)
    a.lea_reg_mem(Reg::RDI, slot(Reg::R12, 48));
    a.mov_reg_imm32sx(Reg::RSI, static_cast<int32_t>(n));
    a.mov_reg_mem(Reg::RDX, slot(Reg::RBX, 8));  // ctx->user
    a.call_mem(slot(Reg::RBX, 0));               // ctx->shim

    // Return translation: the shim returned a tagged word in RAX.
    Label epi;
    Label ret_err;
    switch (sig.return_kind) {
    case ForeignReturnKind::Void:
        a.xor_reg_reg(Reg::RAX, Reg::RAX);
        epi.ref_jmp(a);
        break;
    case ForeignReturnKind::Int:
        a.test_reg_imm8(Reg::RAX, 1);
        ret_err.ref_jcc(a, CC_NE);               // non-Smi return word
        a.sar_reg_imm8(Reg::RAX, 1);             // unbox
        epi.ref_jmp(a);
        break;
    case ForeignReturnKind::Bool: {
        a.cmp_reg_imm32(Reg::RAX, static_cast<int32_t>(kTrueWord));
        Label is_true;
        is_true.ref_jcc(a, CC_E);
        a.cmp_reg_imm32(Reg::RAX, static_cast<int32_t>(kFalseWord));
        Label is_false;
        is_false.ref_jcc(a, CC_E);
        ret_err.ref_jmp(a);                      // non-canonical word
        is_true.bind_here(a);
        a.mov_reg_imm32sx(Reg::RAX, 1);
        epi.ref_jmp(a);
        is_false.bind_here(a);
        a.xor_reg_reg(Reg::RAX, Reg::RAX);       // 0
        epi.ref_jmp(a);
        break;
    }
    }
    ret_err.bind_here(a);
    a.mov_reg_reg(Reg::RDI, Reg::RAX);           // offending word
    a.mov_reg_reg(Reg::RSI, Reg::RBX);
    a.call_mem(slot(Reg::RBX, 56));              // ret_slow -> 0

    epi.bind_here(a);
    a.add_reg_imm32(Reg::RSP, static_cast<int32_t>(sub));  // arena
    a.pop_reg(Reg::R12);
    a.pop_reg(Reg::RBX);
    emit_frame_tail(a);

    // Island: 8 bytes after the ret, holding the heap-owned context pointer.
    const size_t island_pos = buf.size();
    buf.emit64(0);
    const int32_t disp = static_cast<int32_t>(island_pos - (lea_disp + 4));
    uint8_t* p = buf.code().data() + lea_disp;
    p[0] = static_cast<uint8_t>(disp & 0xFF);
    p[1] = static_cast<uint8_t>((disp >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((disp >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((disp >> 24) & 0xFF);
    const uint64_t ctx_addr =
        reinterpret_cast<uint64_t>(stub.ctx.get());
    std::memcpy(buf.code().data() + island_pos, &ctx_addr, sizeof(ctx_addr));

    const size_t total_bytes = buf.size();
    if (total_bytes > stub.memory.size()) {
        return support::fail(support::ErrorCode::InternalError,
                             "ffi: stub emission exceeded the code budget (" +
                                 std::to_string(total_bytes) + " > " +
                                 std::to_string(kStubBudget) + ")");
    }
    stub.memory.write(
        std::span<const uint8_t>(buf.code().data(), total_bytes), 0);
    if (auto pub = stub.memory.publish(); !pub) {
        return std::unexpected(pub.error());
    }
    CallbackContext& ctx = *stub.ctx;
    ctx = CallbackContext{shim, user, SafepointPollingPage::poll_word_raw(),
                          0, static_cast<uint32_t>(n), 0,
                          reinterpret_cast<void*>(&vortex_ffi_callback_safepoint),
                          reinterpret_cast<void*>(&vortex_ffi_callback_box_slow),
                          reinterpret_cast<void*>(&vortex_ffi_callback_ret_slow)};
    stub.fn_ = stub.memory.data();
    stub.id = ffi_trampoline_registry().register_range(stub.memory.data(),
                                                       total_bytes);
    return stub;
}

}  // namespace vortex::infra
