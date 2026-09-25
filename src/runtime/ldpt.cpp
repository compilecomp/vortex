#include "vortex/runtime/ldpt.hpp"

#include <cstddef>
#include <cstring>
#include <utility>

#include "vortex/codegen/x64/assembler.hpp"
#include "vortex/infra/code_range.hpp"
#include "vortex/runtime/object_model.hpp"

namespace vortex::runtime::ldpt {

using support::fail;
using support::ok;
using support::Result;

namespace {

using codegen::x64::Assembler;
using codegen::x64::CC_E;
using codegen::x64::CC_NE;
using ::vortex::codegen::CodeBuffer;
using codegen::x64::Mem;
using codegen::x64::Reg;

// ---- stub layout constants (byte-exact; pinned by the assembly below) -------

// Skeleton prologue: mov rax, imm64 (10) | cmp rax,[rsi] (3) | jne rel32 (6)
// | mov rdi,rsi (3) | call rel32 (5) | ret (1) | pad to 48.
// NOTE (docs/ldpt.md section 1, M2): the primary call is a DIRECT rel32.
// The shared code-range reservation (infra/code_range.hpp) places the arena
// within +-2 GB of every published method, so the steady path is a plain
// direct call — no target-constant indirect, no BTB dependence. A target
// outside the range is a configuration error and fails loudly (the resolver
// path still dispatches it correctly, so the site degrades, never corrupts).
constexpr size_t kSkelJneOff = 13;
constexpr size_t kSkelCallOff = 22;           // call rel32 (E8)
constexpr size_t kSkelRetOff = 27;
constexpr size_t kSkelHoleMovOff = kTrampolinePrologueBytes;      // +7 bytes
constexpr size_t kSkelHoleJmpOff = kTrampolinePrologueBytes + 7;  // +5 bytes

// Mono OOL stub: mov rax, imm64 (10) | cmp rax,[rsi] (3) | jne rel32 (6)
// | mov rdi,rsi (3) | jmp rel32 (5) = 27 bytes.
constexpr size_t kMonoStubBytes = 27;
constexpr size_t kMonoMissJccOff = 15;

// Poly compare chain: kPolyEntryBytes per known type — mov imm64 (10) +
// cmp [rsi] (3) + jne rel32 (6) + mov rdi,rsi (3) + jmp rel32 (5) = 27;
// shared tail = mov rdi, imm32sx (7) + jmp rel32 (5) = 12.
constexpr size_t kPolyEntryBytes = 27;
constexpr size_t kPolyTailBytes = 12;
constexpr size_t kPolyStubMax =
    kPolyEntryBytes * ugb::kIcPolyCapacity + kPolyTailBytes;

// Mega thunk reservation bound (actual size measured at assembly time).
constexpr size_t kMegaThunkMax = 96;

static_assert(kPolyStubMax <= kOolReservationBytes,
              "poly-4 compare chain must fit the out-of-line reservation; "
              "the arena layout is frozen at publish");
static_assert(kMegaThunkMax <= kOolReservationBytes,
              "mega thunk must fit the out-of-line reservation");
static_assert(kSkelHoleJmpOff + 5 <= kTrampolinePrologueBytes + kPatchHoleBytes,
              "hole jmp must stay inside the reserved hole");

/// Scratch assembler whose buffer will land at `base_offset` in the arena.
/// rel32 fixups that target other arena locations go through fix_rel32_out;
/// in-buffer targets use plain relative arithmetic (positions shift
/// uniformly, so buffer-relative rel32 values survive the move).
struct StubAsm {
    CodeBuffer buf;
    Assembler asm_;
    size_t base_offset;  // arena offset where buf will be written
    uint64_t arena_abs;  // absolute address of arena base

    explicit StubAsm(size_t base, uint64_t arena_base)
        : asm_(buf), base_offset(base), arena_abs(arena_base) {}

    /// Patches a rel32 placeholder to point at an arbitrary arena offset.
    void fix_to_arena(size_t ph, size_t target_arena_offset) {
        const int64_t rel = static_cast<int64_t>(target_arena_offset) -
                            static_cast<int64_t>(base_offset + ph + 4);
        const int32_t rel32 = static_cast<int32_t>(rel);
        std::memcpy(buf.code().data() + ph, &rel32, 4);
    }

    /// Absolute address of the placeholder's rel32 payload (next insn).
    uint64_t site_of(size_t ph) const {
        return arena_abs + base_offset + ph + 4;
    }

    /// Patches a rel32 call placeholder to an absolute native address.
    /// Fails loudly when the target is outside rel32 reach — the M2
    /// code-range reservation makes this impossible for published code, and
    /// the failure keeps foreign targets from silently regressing the
    /// primary path into a constant-indirect workaround.
    support::Result<void> fix_branch_abs(size_t ph, uint64_t target_abs) {
        const int64_t delta = static_cast<int64_t>(target_abs) -
                              static_cast<int64_t>(site_of(ph));
        if (delta < -0x80000000ll || delta > 0x7FFFFFFFll) {
            return support::fail(support::ErrorCode::InvalidArgument,
                                 "LDPT target outside rel32 reach: place "
                                 "the arena and the target in the shared "
                                 "code range (docs/ldpt.md section 1)");
        }
        const int32_t rel32 = static_cast<int32_t>(delta);
        std::memcpy(buf.code().data() + ph, &rel32, 4);
        return support::ok();
    }
};

// The receiver's dispatch word: ObjectHeader::klass sits at offset 0
// (runtime/object_model.hpp). The trampoline ABI reads it directly.
constexpr size_t kKlassWordOffset = 0;

}  // namespace

// ---- layout pins -------------------------------------------------------------

static_assert(offsetof(MegaRow, entries) == 8,
              "mega thunk ABI: count u32 at +0 (+4 pad), entries at +8");
static_assert(sizeof(MegaRow::Entry) == 16,
              "mega thunk ABI: the scan strides entries by 16 bytes "
              "(add rdx, 16) and reads the target at +8");
static_assert(kKlassWordOffset == offsetof(vortex::ObjectHeader, klass),
              "the trampoline compares [rsi] against the header klass word");

// ---- site dispatch (C++ mirrors of the machine-code probes) ------------------

// @hot — see the PERF_CONTRACT in runtime/ldpt.hpp.
const PolyTypeEntry* TrampolineSite::lookup(uint64_t klass) const noexcept {
    if (klass == primary_klass && primary_target != 0) {
        static const PolyTypeEntry kPrimarySentinel{};
        // The primary type dispatches through the prologue, never through a
        // PolyTypeEntry; the sentinel lets callers distinguish "known type"
        // from "unknown" without a second state read.
        return &kPrimarySentinel;
    }
    for (uint8_t i = 0; i < poly_count; ++i) {
        if (poly[i].klass == klass) return &poly[i];
    }
    return nullptr;
}

// @hot — C++ mirror of the mega thunk scan (same contract as
// TrampolineSite::lookup; scan bounded by kMegaRowCapacity).
const MegaRow::Entry* MegaRow::lookup(uint64_t klass) const noexcept {
    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].klass == klass) return &entries[i];
    }
    return nullptr;
}

// @cold — one call per NEW megamorphic type per site.
Result<const MegaRow::Entry*> MegaDispatch::publish_entry(uint64_t klass,
                                                          uint64_t target) {
    const MegaRow* current = active.load(std::memory_order_acquire);
    if (current != nullptr) {
        if (const auto* existing = current->lookup(klass)) return existing;
        if (current->count >= kMegaRowCapacity) {
            return fail(support::ErrorCode::RuntimeError,
                        "megamorphic row overflow (docs/ldpt.md section 3)");
        }
    }
    auto row = std::make_unique<MegaRow>();
    if (current != nullptr) *row = *current;
    row->entries[row->count] = {klass, target};
    ++row->count;
    MegaRow* published = row.get();
    retired.push_back(std::move(row));
    // Release store pairs with the thunk's plain load: on x86-64 every load
    // has acquire semantics, so entry data is visible before the swizzle
    // (CEM-26 section 14: release/acquire, never seq_cst).
    active.store(published, std::memory_order_release);
    return &published->entries[published->count - 1];
}

// ---- manager -----------------------------------------------------------------

LdptManager::LdptManager(LdptConfig config,
                         infra::HandshakeManager* handshake,
                         infra::DependencyGraph* deps)
    : handshake_(handshake), deps_(deps), config_(config) {
    auto arena = infra::PatchArena::allocate(config_.arena_bytes,
                                             config_.code_range);
    if (arena) {
        arena_ = std::move(*arena);
        emit_resolver_thunk();
    }
}

// @cold — emitted once per arena at construction.
void LdptManager::emit_resolver_thunk() {
    auto carved = arena_.carve(kMegaThunkMax);
    if (!carved) return;  // arena too small for the thunk: unusable
    thunk_offset_ = *carved;
    StubAsm s(thunk_offset_, reinterpret_cast<uint64_t>(arena_.at(0)));
    // ABI in: rdi = site handle, rsi = receiver (set by the hole).
    // ABI out: rdi = manager, esi = handle, rdx = receiver.
    s.asm_.mov_reg_reg(Reg::R8, Reg::RDI);   // handle -> r8
    s.asm_.mov_reg_reg(Reg::R9, Reg::RSI);   // receiver -> r9
    s.asm_.mov_reg_imm64(Reg::RDI, reinterpret_cast<uint64_t>(this));
    s.asm_.mov_reg_reg(Reg::RSI, Reg::R8);   // handle -> rsi
    s.asm_.mov_reg_reg(Reg::RDX, Reg::R9);   // receiver -> rdx
    s.asm_.mov_reg_imm64(
        Reg::RAX, reinterpret_cast<uint64_t>(&LdptManager::resolve_miss));
    s.asm_.jmp_reg(Reg::RAX);
    (void)arena_.write(s.buf.code(), thunk_offset_);
}

support::Result<std::vector<uint8_t>> LdptManager::assemble_skeleton(
    const TrampolineSite& site) const {
    StubAsm s(site.trampoline_offset,
              reinterpret_cast<uint64_t>(arena_.at(0)));
    s.asm_.mov_reg_imm64(Reg::RAX, site.primary_klass);
    s.asm_.cmp_reg_mem(Reg::RAX, Mem{Reg::RSI, Reg::RSP, 0, 0});
    const size_t jcc = s.asm_.placeholder_jcc(CC_NE);
    s.asm_.mov_reg_reg(Reg::RDI, Reg::RSI);
    // Direct rel32 call to the primary target (docs/ldpt.md section 1):
    // the code-range reservation guarantees reachability; a foreign target
    // fails here instead of degrading the primary path to an indirect.
    const size_t call = s.asm_.placeholder_call();
    if (auto r = s.fix_branch_abs(call, site.primary_target); !r) {
        return std::unexpected(std::move(r).error());
    }
    s.asm_.ret();
    s.asm_.nop(kTrampolinePrologueBytes - kSkelRetOff - 1);
    // Hole: mov rdi, imm32sx handle (7) + jmp rel32 thunk (5) + nop pad.
    s.asm_.mov_reg_imm32sx(Reg::RDI, static_cast<int32_t>(site.handle));
    const size_t hjmp = s.asm_.placeholder_jmp();
    s.asm_.nop(kPatchHoleBytes - 12);
    s.fix_to_arena(jcc, site.hole_offset);
    s.fix_to_arena(hjmp, thunk_offset_);
    return std::move(s.buf.code());
}

Result<TrampolineSite*> LdptManager::emit_skeleton(
    uint32_t method_id, uint32_t instruction_index, uint64_t primary_klass,
    uint64_t primary_target) {
    if (arena_.published()) {
        return fail(support::ErrorCode::InvalidArgument,
                    "LDPT arena is published: emission is once per manager");
    }
    if (primary_target == 0) {
        return fail(support::ErrorCode::InvalidArgument,
                    "skeleton requires a primary target");
    }
    TrampolineSite site;
    site.site_id = ugb::make_site_id(method_id, instruction_index);
    site.method_id = method_id;
    site.instruction_index = instruction_index;
    site.primary_klass = primary_klass;
    site.primary_target = primary_target;
    site.state = TrampolineState::Mono;

    auto carved = arena_.carve(kTrampolineBytes);
    if (!carved) return std::unexpected(std::move(carved).error());
    site.trampoline_offset = *carved;
    site.hole_offset = site.trampoline_offset + kTrampolinePrologueBytes;
    site.ool_offset = site.hole_offset + kPatchHoleBytes;
    site.handle = static_cast<uint32_t>(sites_.size());

    auto bytes = assemble_skeleton(site);
    if (!bytes) return std::unexpected(std::move(bytes).error());
    if (auto w = arena_.write(*bytes, site.trampoline_offset); !w) {
        return std::unexpected(std::move(w).error());
    }
    // docs/ldpt.md 4A: the site assumes its targets stay put; unload or
    // recompile invalidates through the dependency graph and the hole
    // degrades back to the resolver instead of dangling.
    if (deps_ != nullptr) {
        deps_->register_assumption(
            infra::Assumption{infra::AssumptionKind::MethodFinal, method_id},
            infra::DependentRegionRef{method_id, instruction_index, true});
    }
    sites_.push_back(site);
    mega_rows_.emplace_back();
    ++stats_.skeletons_emitted;
    ++stats_.mono_patches;  // the primary type's marshalling is live now
    return &sites_.back();
}

// @cold — the patch protocol: one session per escalation batch. Reached once
// per (site, type) pair; steady-state execution never re-enters C++.
// PERF_CONTRACT:
// BUDGET: <= 20us per session (2 mprotect flips + handshake + <= 272 bytes
//         of memcpy); amortized to zero by the patched steady state
// READS: scratch buffers only
// WRITES: bounded by hole + OOL reservation per site
// BRANCHES: failure paths only
// CACHE: cold pages + the patched lines
// PERF_PERMIT PERF-005 (mprotect pair): the W^X law makes every code write a
//          two-flip session; batching per escalation bounds the cost.
// OWNER: @vortex/rt (registered in docs/cem26.md section 5)
Result<void> LdptManager::patch_session(
    const std::vector<std::pair<size_t, std::span<const uint8_t>>>& writes) {
    // M:N safe-point freeze (docs/ldpt.md section 2). M1 binds the contract
    // stub: request/acknowledge record the protocol; the M2 scheduler binds
    // cooperative yields. The ordering IS the contract: no arena write may
    // happen outside request..acknowledge.
    if (handshake_ != nullptr) {
        if (auto r = handshake_->request(infra::SuspensionKind::Gc, 0); !r) {
            return r;
        }
    }
    if (auto r = arena_.begin_session(); !r) {
        if (handshake_ != nullptr) (void)handshake_->acknowledge(0);
        return r;
    }
    for (const auto& [offset, bytes] : writes) {
        if (auto r = arena_.patch(bytes, offset); !r) {
            (void)arena_.end_session();
            if (handshake_ != nullptr) (void)handshake_->acknowledge(0);
            return r;
        }
    }
    const bool ok_end = arena_.end_session().has_value();
    if (handshake_ != nullptr) (void)handshake_->acknowledge(0);
    if (!ok_end) {
        return fail(support::ErrorCode::InternalError,
                    "patch session end failed");
    }
    return ok();
}

Result<size_t> LdptManager::assemble_ool_stub(const TrampolineSite& site,
                                              uint8_t* out, size_t cap,
                                              size_t* out_size) const {
    StubAsm s(site.ool_offset, reinterpret_cast<uint64_t>(arena_.at(0)));
    const auto receiver_klass_cmp = [&s] {
        s.asm_.cmp_reg_mem(Reg::RAX, Mem{Reg::RSI, Reg::RSP, 0, 0});
    };

    size_t size = 0;
    if (site.state == TrampolineState::Mono) {
        s.asm_.mov_reg_imm64(Reg::RAX, site.primary_klass);
        receiver_klass_cmp();
        const size_t jcc = s.asm_.placeholder_jcc(CC_NE);
        s.fix_to_arena(jcc, thunk_offset_);  // miss -> resolver thunk
        s.asm_.mov_reg_reg(Reg::RDI, Reg::RSI);
        const size_t jmp = s.asm_.placeholder_jmp();
        if (auto r = s.fix_branch_abs(jmp, site.primary_target); !r) {
            return std::unexpected(std::move(r).error());
        }
        size = kMonoStubBytes;
    } else if (site.state == TrampolineState::Poly) {
        for (uint8_t i = 0; i < site.poly_count; ++i) {
            s.asm_.mov_reg_imm64(Reg::RAX, site.poly[i].klass);
            receiver_klass_cmp();
            const size_t jcc = s.asm_.placeholder_jcc(CC_NE);
            // EVERY miss-jcc targets (i+1)*stride: the next entry for the
            // first N-1 entries, the resolver tail for the last one. An
            // unfixed last-entry jcc would fall through into its own
            // match-dispatch and dispatch the PREVIOUS entry's target.
            s.fix_to_arena(jcc, site.ool_offset + (i + 1) * kPolyEntryBytes);
            s.asm_.mov_reg_reg(Reg::RDI, Reg::RSI);
            const size_t jmp = s.asm_.placeholder_jmp();
            if (auto r = s.fix_branch_abs(jmp, site.poly[i].target); !r) {
                return std::unexpected(std::move(r).error());
            }
        }
        s.asm_.mov_reg_imm32sx(Reg::RDI, static_cast<int32_t>(site.handle));
        const size_t tail = s.asm_.placeholder_jmp();
        s.fix_to_arena(tail, thunk_offset_);
        size = kPolyEntryBytes * site.poly_count + kPolyTailBytes;
    } else if (site.state == TrampolineState::Mega) {
        // Hole carries rdi = handle; the row pointer lives in THIS SITE'S
        // MegaDispatch (plain RW data, atomic swizzle).
        const void* row_slot = &mega_rows_[site.handle].active;
        s.asm_.mov_reg_imm64(Reg::RAX, reinterpret_cast<uint64_t>(row_slot));
        s.asm_.mov_reg_mem(Reg::RAX, Mem{Reg::RAX, Reg::RSP, 0, 0});
        s.asm_.mov_reg32_mem(Reg::R10, Mem{Reg::RAX, Reg::RSP, 0, 0});
        s.asm_.add_reg_imm32(Reg::RAX, 8);  // entries base
        s.asm_.mov_reg_reg(Reg::RDX, Reg::RAX);
        const size_t loop_top = s.buf.size();
        s.asm_.mov_reg_mem(Reg::RCX, Mem{Reg::RDX, Reg::RSP, 0, 0});
        s.asm_.cmp_reg_mem(Reg::RCX, Mem{Reg::RSI, Reg::RSP, 0, 0});
        const size_t hit_jcc = s.asm_.placeholder_jcc(CC_E);
        s.asm_.add_reg_imm32(Reg::RDX, 16);
        s.asm_.dec_reg(Reg::R10);
        const size_t loop_jcc = s.asm_.placeholder_jcc(CC_NE);
        s.fix_to_arena(loop_jcc, site.ool_offset + loop_top);
        // Miss (row lag behind a swizzle): degrade to the resolver —
        // correctness preserved, latency bounded.
        s.asm_.mov_reg_imm32sx(Reg::RDI, static_cast<int32_t>(site.handle));
        const size_t miss = s.asm_.placeholder_jmp();
        s.fix_to_arena(miss, thunk_offset_);
        // Hit: rdi = receiver, rax = entry.target, tail-jmp.
        const size_t hit_off = s.buf.size();
        s.asm_.mov_reg_reg(Reg::RDI, Reg::RSI);
        s.asm_.mov_reg_mem(Reg::RAX, Mem{Reg::RDX, Reg::RSP, 0, 8});
        s.asm_.jmp_reg(Reg::RAX);
        s.fix_to_arena(hit_jcc, site.ool_offset + hit_off);
        size = s.buf.size();
    } else {
        return fail(support::ErrorCode::InvalidArgument,
                    "no OOL stub for the site state");
    }
    if (size > cap) {
        return fail(support::ErrorCode::InternalError,
                    "OOL stub exceeds its reservation");
    }
    *out_size = size;
    std::memcpy(out, s.buf.code().data(), size);
    return size;
}

Result<void> LdptManager::escalate(TrampolineSite& site, uint64_t klass,
                                   uint64_t target) {
    if (site.lookup(klass) != nullptr) {
        return ok();  // raced patch won or primary re-entry: nothing to do
    }
    const TrampolineState prior = site.state;
    std::vector<std::pair<size_t, std::span<const uint8_t>>> writes;
    std::vector<uint8_t> stub(kOolReservationBytes, 0x90);
    std::vector<uint8_t> hole;  // OUTLIVES the span stored in `writes`
    size_t stub_size = 0;

    if (site.state == TrampolineState::Mono ||
        site.state == TrampolineState::Resolver) {
        // First alternative type (or re-patch after invalidation):
        // poly[0] = primary, poly[1] = the new type.
        site.poly[0] = {site.primary_klass, site.primary_target};
        site.poly[1] = {klass, target};
        site.poly_count = 2;
        site.state = TrampolineState::Poly;
    } else if (site.state == TrampolineState::Poly &&
               site.poly_count < ugb::kIcPolyCapacity) {
        site.poly[site.poly_count++] = {klass, target};
    } else if (site.state == TrampolineState::Poly) {
        // Poly overflow -> megamorphic. Data row FIRST (no session), then
        // the thunk + hole swap inside one session: the hole only flips
        // after the row is visible.
        for (uint8_t i = 0; i < site.poly_count; ++i) {
            (void)mega_rows_[site.handle].publish_entry(site.poly[i].klass,
                                                        site.poly[i].target);
        }
        auto added =
            mega_rows_[site.handle].publish_entry(klass, target);
        if (!added) return std::unexpected(std::move(added).error());
        site.state = TrampolineState::Mega;
    } else if (site.state == TrampolineState::Mega) {
        // Pure data update: atomic pointer swizzle, NO safe point, NO
        // session (docs/ldpt.md section 3).
        auto added =
            mega_rows_[site.handle].publish_entry(klass, target);
        if (!added) return std::unexpected(std::move(added).error());
        ++stats_.misses_resolved;
        return ok();
    } else {
        return fail(support::ErrorCode::InvalidArgument,
                    "resolver hit a Resolver-state site");
    }

    auto assembled =
        assemble_ool_stub(site, stub.data(), stub.size(), &stub_size);
    if (!assembled) return std::unexpected(std::move(assembled).error());
    writes.emplace_back(site.ool_offset,
                        std::span<const uint8_t>(stub.data(), stub_size));

    // The hole rewires on the FIRST escalation only: it becomes a bare jmp
    // to the OOL stub, which later escalations rewrite in place. (The state
    // has already advanced, so branch on the PRIOR state.)
    if (prior == TrampolineState::Mono ||
        prior == TrampolineState::Resolver) {
        StubAsm h(site.hole_offset,
                  reinterpret_cast<uint64_t>(arena_.at(0)));
        const size_t j = h.asm_.placeholder_jmp();
        h.fix_to_arena(j, site.ool_offset);
        hole.assign(kPatchHoleBytes, 0x90);
        std::memcpy(hole.data(), h.buf.code().data(), 5);
        writes.emplace_back(
            site.hole_offset, std::span<const uint8_t>(hole));
    } else if (site.state == TrampolineState::Mega) {
        StubAsm h(site.hole_offset,
                  reinterpret_cast<uint64_t>(arena_.at(0)));
        h.asm_.mov_reg_imm32sx(Reg::RDI, static_cast<int32_t>(site.handle));
        const size_t j = h.asm_.placeholder_jmp();
        h.fix_to_arena(j, site.ool_offset);
        hole.assign(kPatchHoleBytes, 0x90);
        std::memcpy(hole.data(), h.buf.code().data(), 12);
        writes.emplace_back(
            site.hole_offset, std::span<const uint8_t>(hole));
    }
    // Poly: the hole still points at the OOL base — only the stub rewrites.

    if (auto r = patch_session(writes); !r) {
        return std::unexpected(std::move(r).error());
    }
    switch (site.state) {
    case TrampolineState::Mono: ++stats_.mono_patches; break;
    case TrampolineState::Poly: ++stats_.poly_patches; break;
    case TrampolineState::Mega: ++stats_.mega_escalations; break;
    default: break;
    }
    ++stats_.misses_resolved;
    return ok();
}

// @cold — first miss per (site, type). PERF_PERMIT PERF-007 (see header):
// codegen + page flip amortize to zero against the patched steady state.
// OOM during escalation terminates (deliberate: allocator failure inside a
// patch protocol is not recoverable in-place).
uint64_t LdptManager::resolve_miss(LdptManager* manager, uint32_t handle,
                                   void* receiver) noexcept {
    if (manager == nullptr || handle >= manager->sites_.size()) return 0;
    TrampolineSite& site = manager->sites_[handle];
    uint64_t klass = 0;
    std::memcpy(&klass, receiver, sizeof(klass));
    if (klass == site.primary_klass && site.primary_target != 0 &&
        site.state != TrampolineState::Resolver) {
        return reinterpret_cast<TargetFn>(site.primary_target)(receiver);
    }
    if (const PolyTypeEntry* known = site.lookup(klass)) {
        // Raced patch from another thread won; dispatch directly.
        return reinterpret_cast<TargetFn>(known->target)(receiver);
    }
    if (manager->target_resolver_ == nullptr) return 0;
    bool resolved = false;
    const uint64_t target =
        manager->target_resolver_(manager->resolver_user_, site.method_id,
                                  site.instruction_index, receiver, &resolved);
    if (!resolved || target == 0) return 0;
    auto esc = manager->escalate(site, klass, target);
    if (!esc) {
        // Native patching unavailable (foreign target, patch protocol
        // failure): dispatch through C++ so the RESULT is always correct;
        // the site stays on resolver dispatch (QoS degradation only).
        return reinterpret_cast<TargetFn>(target)(receiver);
    }
    return reinterpret_cast<TargetFn>(target)(receiver);
}

Result<TrampolineSite*> LdptManager::invalidate(uint64_t site_id) {
    TrampolineSite* site = find_site_mutable(site_id);
    if (site == nullptr) return nullptr;
    StubAsm h(site->hole_offset,
              reinterpret_cast<uint64_t>(arena_.at(0)));
    // Restore the original resolver hole: handle + tail-jmp to the thunk.
    h.asm_.mov_reg_imm32sx(Reg::RDI, static_cast<int32_t>(site->handle));
    const size_t j = h.asm_.placeholder_jmp();
    h.fix_to_arena(j, thunk_offset_);
    std::vector<uint8_t> hole(kPatchHoleBytes, 0x90);
    std::memcpy(hole.data(), h.buf.code().data(), 12);
    std::vector<std::pair<size_t, std::span<const uint8_t>>> writes;
    writes.emplace_back(site->hole_offset, std::span<const uint8_t>(hole));
    // (hole outlives patch_session within this scope — safe here.)
    if (auto r = patch_session(writes); !r) {
        return std::unexpected(std::move(r).error());
    }
    site->state = TrampolineState::Resolver;
    ++stats_.invalidations;
    return site;
}

Result<void> LdptManager::invalidate_batch(
    const infra::InvalidationBatch& batch) {
    for (const auto& region : batch.immediate) {
        const uint64_t id =
            ugb::make_site_id(region.method_id, region.region_id);
        if (find_site(id) != nullptr) {
            if (auto r = invalidate(id); !r) {
                return std::unexpected(std::move(r).error());
            }
        }
    }
    // Lazy regions degrade on next execution — the resolver path IS the
    // lazy handler, so nothing to patch now (docs/ldpt.md section 4A).
    return ok();
}

const TrampolineSite* LdptManager::find_site(
    uint64_t site_id) const noexcept {
    for (const auto& s : sites_) {
        if (s.site_id == site_id) return &s;
    }
    return nullptr;
}

TrampolineSite* LdptManager::find_site_mutable(uint64_t site_id) noexcept {
    for (auto& s : sites_) {
        if (s.site_id == site_id) return &s;
    }
    return nullptr;
}

}  // namespace vortex::runtime::ldpt
