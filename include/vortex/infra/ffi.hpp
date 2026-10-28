// CEP:FILE: include/vortex/infra/ffi.hpp
// CEP:WHAT: Infra 5 public surface (docs/infrastructure/05-ffi.md): the
//           handle table and pinning regions over the moving ICGGC heap
//           (M0), the trampoline generator contract (M6-c — synthesized
//           SysV x86-64 stubs through the ONE W^X publish path with
//           normative context layouts), the FfiTrampolineRegistry the
//           unified unwinder consumes, and the mixed-stack UnifiedUnwinder
//           + Rule-132 exception bridge.
// CEP:WHY: The native boundary is machine code — the context offsets, the
//           stub entry types, and the registry pairing are ABI contracts
//           pinned here once (static_asserts) and consumed by both the
//           emitter and the walker.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: Emit-time refusals are named Result rejects; runtime
//           boundary failures ride FfiBoundaryError bits through the
//           contexts (the C++ wrapper converts them to named Results).
// CEP:ASSUMES: Linux x86-64 SysV LP64; TaggedValue/ObjectHeader layout
//           facts static_assert-pinned in their headers; contexts are
//           generator-filled before first call.
// CEP:COST: @cold emit paths; @warm stub fast paths are straight-line
//           machine code (one poll-word load per call).
// CEP:EVIDENCE: tests/test_ffi.cpp (pins, parity, walk, refusals).
// Infra 5 — Native Interop (FFI) & ABI translation
// (docs/infrastructure/05-ffi.md).
//
// M0 implemented the HandleTable (indirection over the moving ICGGC heap).
// M6-c lands the two contract-stubbed mechanisms for real: the trampoline
// generator (synthesized SysV x86-64 stubs through the ONE W^X publish path,
// assembled with the M1 assembler — docs/infrastructure/05-ffi.md normative
// sections) and the unified unwinder (mixed JIT/trampoline/native frame walk
// over the published-code registry + the named-failure exception bridge).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "vortex/infra/security.hpp"
#include "vortex/support/result.hpp"
#include "vortex/support/tagged_value.hpp"

namespace vortex::infra {

enum class HandleScope : uint8_t { Global, Weak, Local };

/// Indirection table: native code holds handles, never raw pointers into the
/// concurrently-moving heap (docs/infrastructure/05 section: object pinning &
/// handle tables).
class HandleTable {
public:
    /// One handle slot (docs/gc-icggc.md 8.6: the collector walks entries to
    /// scan/update roots and clears dead weak handles).
    struct HandleEntry {
        void* object = nullptr;
        HandleScope scope = HandleScope::Local;
        bool alive = false;
    };

    support::Result<uint32_t> create(void* object, HandleScope scope);

    /// Resolves a handle; fails for freed handles. Weak handles to objects
    /// the collector cleared resolve to a null value (named nullptr, not an
    /// error — Rule 85's valid-state boundary).
    support::Result<void*> resolve(uint32_t handle) const;

    void destroy(uint32_t handle);

    /// Frees all Local handles (scope exit).
    size_t destroy_scope(HandleScope scope);

    /// GC integration (docs/gc-icggc.md 8.6): every live entry, mutable.
    // CEP:WHAT: Walks every handle entry (including dead slots) mutably.
    // CEP:WHY: docs/gc-icggc.md 8.6 — the collector scans/updates roots and clears dead weak handles.
    // CEP:STATUS: complete
    // CEP:FAILURE: none (read/write walk).
    // CEP:ASSUMES: called only at pauses (single mutator).
    // CEP:COST: @warm. O(entries).
    // CEP:EVIDENCE: gc_compaction_preserves_identity_under_ffi_pinning (weak clearing).
    template <typename Fn>
    void for_each_entry(Fn&& fn) {
        for (uint32_t i = 0; i < entries_.size(); ++i) fn(i, entries_[i]);
    }

    size_t live_count() const noexcept { return live_; }

private:
    std::vector<HandleEntry> entries_;
    std::vector<uint32_t> free_list_;
    size_t live_ = 0;
};

/// RAII pinning region: forbids the GC from moving specific objects during a
/// native call (the GC consults registered regions — docs/gc-icggc.md 8.6:
/// pinned objects are roots, never evacuated; a pinned young object promotes
/// with its pin entry updated in place, identity preserved).
class PinningRegion {
public:
    void pin(void* object) { pinned_.push_back(object); }
    void unpin(void* object);
    bool is_pinned(const void* object) const noexcept;
    const std::vector<void*>& pinned() const noexcept { return pinned_; }
    /// Collector access: the young evacuation updates pin entries when a
    /// pinned young object is promoted (docs/gc-icggc.md 8.1).
    std::vector<void*>& pinned_mut() noexcept { return pinned_; }

private:
    std::vector<void*> pinned_;
};

// ---------------------------------------------------------------------------
// Trampoline generator (M6-c, docs/infrastructure/05-ffi.md normative
// sections). Synthesized SysV x86-64 stubs; AAPCS64 is the Post-M6 ARM64
// item. Emission is a pure function of (signature, context pointers):
// byte-identical stubs for identical inputs (no entropy, no baked addresses —
// the poll word and the callees are reached through context fields).
// ---------------------------------------------------------------------------

enum class AbiKind : uint8_t { SystemVX86_64, Aapcs64 };

/// Ordered per-argument kinds. The tagged-value translation rules per kind
/// are normative in docs/infrastructure/05-ffi.md.
enum class ForeignArgKind : uint8_t {
    Int,    ///< guest Smi <-> native int64 (int63 range, shift-by-1 encoding)
    F64,    ///< guest boxed double <-> native double (xmm, payload at +24)
    Ptr,    ///< guest pointer-like word <-> 16-byte-aligned object address
    Bool,   ///< guest canonical true/false words <-> native 0/1
    Null,   ///< canonical null word (callback signatures with nullable slots)
};

enum class ForeignReturnKind : uint8_t { Void, Int, Bool };
static_assert(std::is_trivially_copyable_v<TaggedValue> && sizeof(TaggedValue) == 8,
              "the stubs return TaggedValue in RAX — single-word SysV INTEGER class");
// F64 returns are emit-time refusals in this batch: boxing a double requires
// a heap allocation the stub must not perform (docs/infrastructure/05-ffi.md,
// return translation). The allocation-integrated form ships with the
// emitted-poll/J4 batch.

struct ForeignSignature {
    AbiKind abi = AbiKind::SystemVX86_64;
    std::vector<ForeignArgKind> args;
    ForeignReturnKind return_kind = ForeignReturnKind::Void;
};

/// Boundary-error bits recorded by the stubs' slow-path helpers. The stubs
/// cannot return Result (machine code); the C++ caller converts a non-zero
/// `error` after the call into a named Result failure (Rule 82: named
/// failures, never silent substitutes).
enum FfiBoundaryError : uint32_t {
    kFfiErrNone = 0,
    kFfiErrType = 1u << 0,      ///< non-Smi at an Int slot / bad tag at F64/Ptr
    kFfiErrOverflow = 1u << 1,  ///< int63 box overflow (return or callback arg)
    kFfiErrForeign = 1u << 2,   ///< foreign side reported a trap (callback shim)
};

/// Context the guest→native stub reads and the slow-path helpers write.
/// Field offsets are normative (the machine code addresses them by fixed
/// displacement) and pinned by static_asserts below. The helper slots are
/// filled by the generator — routing every callee through the context keeps
/// the emitted bytes address-free (byte-identical stubs, docs/infrastructure/
/// 05-ffi.md deterministic-compile section).
struct TrampolineContext {
    void* foreign_fn = nullptr;   ///< +0   the native callee
    void* user = nullptr;         ///< +8   embedder data, untouched by the stub
    void* poll_word = nullptr;    ///< +16  raw global poll word (SafepointPollingPage)
    uint32_t error = 0;           ///< +24  FfiBoundaryError bits
    uint32_t safepoint_ticks = 0; ///< +28  boundary polls taken
    /// +32  void(*)(TrampolineContext*) — invoked when the poll word is armed
    void* safepoint_hook = nullptr;
    /// +40  uint64_t(*)(int64_t, TrampolineContext*) — int63 box overflow
    void* box_smi_slow = nullptr;
    /// +48  uint64_t(*)(uint32_t, TrampolineContext*) — boundary type error
    void* type_error_slow = nullptr;
};
static_assert(offsetof(TrampolineContext, poll_word) == 16 &&
                  offsetof(TrampolineContext, error) == 24 &&
                  offsetof(TrampolineContext, safepoint_hook) == 32 &&
                  offsetof(TrampolineContext, box_smi_slow) == 40 &&
                  offsetof(TrampolineContext, type_error_slow) == 48,
              "stub machine code addresses context fields by fixed offset");

/// Context the native→guest callback stub reads and the helpers write.
/// Same normative-offset discipline as TrampolineContext.
struct CallbackContext {
    void* shim = nullptr;       ///< +0  extern "C" uint64_t(const TaggedValue*, size_t, void*)
    void* user = nullptr;       ///< +8
    void* poll_word = nullptr;  ///< +16 polled BEFORE re-entering the engine
    uint32_t error = 0;         ///< +24 FfiBoundaryError bits
    uint32_t arity = 0;         ///< +28 fixed at emit (shim arg count)
    uint32_t safepoint_ticks = 0;  ///< +32 boundary polls taken
    /// +40  void(*)(CallbackContext*) — invoked when the poll word is armed
    void* safepoint_hook = nullptr;
    /// +48  void(*)(int64_t, CallbackContext*, TaggedValue* slot) — box overflow
    void* box_slow = nullptr;
    /// +56  uint64_t(*)(uint64_t word, CallbackContext*) — return type error
    void* ret_slow = nullptr;
};
static_assert(offsetof(CallbackContext, error) == 24 &&
                  offsetof(CallbackContext, arity) == 28 &&
                  offsetof(CallbackContext, safepoint_hook) == 40 &&
                  offsetof(CallbackContext, box_slow) == 48 &&
                  offsetof(CallbackContext, ret_slow) == 56,
              "stub machine code addresses context fields by fixed offset");

/// Process-wide registry of published trampoline code ranges. Every emit
/// registers its published range; the unified unwinder consumes it to
/// classify trampoline frames. Trampolines keep RBP-chained frames and hold
/// NO tagged live state across the foreign call (arguments are translated
/// into registers before the boundary), so identification suffices — no GC
/// maps.
class FfiTrampolineRegistry {
public:
    struct Range {
        uintptr_t lo = 0;
        uintptr_t hi = 0;  // exclusive
        uint32_t id = 0;
    };

    uint32_t register_range(const void* code, size_t bytes);
    void unregister(uint32_t id);
    /// Returns the range whose span contains `pc`, or nullptr.
    const Range* lookup(uintptr_t pc) const noexcept;
    size_t count() const noexcept { return ranges_.size(); }

private:
    std::vector<Range> ranges_;
    uint32_t next_id_ = 1;
};

FfiTrampolineRegistry& ffi_trampoline_registry() noexcept;

/// A published guest→native stub. Owns its RX pages (WritableCodeMemory
/// move-only) and its context; the entry is called with the tagged args
/// array and the context pointer. The C++ caller converts `ctx.error` into
/// a named failure after the call.
struct GuestToNativeStub {
    GuestToNativeStub() = default;
    GuestToNativeStub(WritableCodeMemory&& mem) {
        memory = std::move(mem);
    }
    ~GuestToNativeStub();
    GuestToNativeStub(GuestToNativeStub&& other) noexcept;
    GuestToNativeStub& operator=(GuestToNativeStub&& other) noexcept;
    GuestToNativeStub(const GuestToNativeStub&) = delete;
    GuestToNativeStub& operator=(const GuestToNativeStub&) = delete;

    WritableCodeMemory memory{};
    TaggedValue (*entry)(const TaggedValue* args, TrampolineContext* ctx) =
        nullptr;
    TrampolineContext ctx{};
    uint32_t id = 0;  // FfiTrampolineRegistry id; 0 when moved-from/absent
    /// Batch-e managed sanitizers: the argument indices whose kind is Ptr
    /// (copied from the signature at generation). `validate_ptr_args` is
    /// the C++ boundary check the machine-code type gates cannot do — a
    /// Ptr arg's object base must be addressable in the guest shadow
    /// before foreign code receives it (docs/infrastructure/06).
    std::vector<uint8_t> ptr_arg_slots;
    support::Result<void> validate_ptr_args(const TaggedValue* args,
                                            size_t nargs) const noexcept;
};

/// A published native→guest callback stub. `fn_ptr` is the SysV entry handed
/// to native code (integer args in registers per the signature's Int/Bool/
/// Null slots; F64/Ptr arguments are emit-time refusals in this batch).
/// The context is HEAP-OWNED: the stub's rip-relative data island bakes the
/// context's address into the machine code, so the address must survive
/// moves of this wrapper (unique_ptr — Rule 83 identity discipline).
struct NativeToGuestStub {
    NativeToGuestStub() = default;
    NativeToGuestStub(WritableCodeMemory&& mem) {
        memory = std::move(mem);
    }
    ~NativeToGuestStub();
    NativeToGuestStub(NativeToGuestStub&& other) noexcept;
    NativeToGuestStub& operator=(NativeToGuestStub&& other) noexcept;
    NativeToGuestStub(const NativeToGuestStub&) = delete;
    NativeToGuestStub& operator=(const NativeToGuestStub&) = delete;

    WritableCodeMemory memory{};
    void* fn_ptr() const noexcept { return fn_; }
    std::unique_ptr<CallbackContext> ctx{new CallbackContext{}};
    uint32_t id = 0;

    void* fn_ = nullptr;
};

class TrampolineGenerator {
public:
    /// Refusals (named, no side effects): F64 return kind; more than 6
    /// integer or more than 6 F64 arguments (stack-arg spill is the
    /// AAPCS64/stack-arg batch); non-SysV abi in this batch.
    static support::Result<GuestToNativeStub> emit_guest_to_native(
        const ForeignSignature& sig, void* foreign_fn, void* user);

    /// Refusals: F64/Ptr arguments (both need heap allocation or pin
    /// discipline the callback context does not have yet); more than 6
    /// integer arguments; non-SysV abi.
    static support::Result<NativeToGuestStub> emit_native_to_guest(
        const ForeignSignature& sig, void* shim, void* user);
};

// ---------------------------------------------------------------------------
// Unified unwinding (M6-c). One walker names every frame of a mixed stack:
// JIT (published-code registry), trampoline (FfiTrampolineRegistry), native
// (dladdr). C++ exceptions never cross a boundary frame — they are
// translated to named ForeignBoundary failures (Rule 132; the exception
// register tracks the boundary semantics).
// ---------------------------------------------------------------------------

struct UnifiedFrame {
    enum class Kind : uint8_t { Jit, Trampoline, Native };
    Kind kind = Kind::Native;
    uintptr_t ip = 0;
    uintptr_t rbp = 0;
    /// Jit/Trampoline: ip - range base (identity within the published code;
    /// the GcCodeRecord itself carries no method id — consumers resolve the
    /// record through the registries when they need more than the offset).
    uint32_t pc_offset = 0;
    const char* origin = nullptr;  ///< dladdr module/symbol hint; may be null
};

class UnifiedUnwinder {
public:
    /// Walks the RBP chain from (ret_pc, rbp) — the return address of the
    /// frame being left and ITS frame pointer, i.e. the pair
    /// __builtin_return_address/__builtin_frame_address of the caller yield.
    /// Terminates on null RBP, a non-monotonic move, or max_frames. Frames
    /// the registries cannot classify report Kind::Native with dladdr hints.
    static std::vector<UnifiedFrame> walk(uintptr_t rbp, uintptr_t ret_pc,
                                          size_t max_frames = 256);

    /// Captures (frame address, return address) of the immediate caller and
    /// walks from there. The capturing frame itself is NOT included.
    static std::vector<UnifiedFrame> walk_current(size_t max_frames = 256);
};

/// Exception bridge (Rule 132): invokes `fn` under the boundary's
/// try/catch; ANY escaping C++ exception becomes a named ForeignBoundary
/// failure carrying the exception text. Foreign exceptions never unwind
/// through guest frames.
template <typename Fn>
auto ffi_boundary_call(const char* boundary, Fn&& fn)
    -> support::Result<decltype(fn())> {
    try {
        return support::Result<decltype(fn())>(fn());
    } catch (const std::exception& e) {
        return support::fail(support::ErrorCode::ForeignBoundary,
                             std::string(boundary) + ": " + e.what());
    } catch (...) {
        return support::fail(support::ErrorCode::ForeignBoundary,
                             std::string(boundary) + ": unknown foreign throw");
    }
}

}  // namespace vortex::infra
