// LDPT (Lazy-Devirtualized Patch Trampolines) tests — docs/ldpt.md:
//   - W^X patch-arena lifecycle (no implicit patching, sessions guarded)
//   - skeleton emission + primary-type direct dispatch
//   - first-miss patch protocol (resolver -> Mono) with real execution
//   - Poly escalation and megamorphic data-only swizzling
//   - dependency-driven invalidation degrades back to the resolver
#include "vortex_test.hpp"

#include <cstring>

#include "vortex/infra/dependency.hpp"
#include "vortex/infra/threading.hpp"
#include "vortex/runtime/ldpt.hpp"

using namespace vortex;
using namespace vortex::runtime::ldpt;

namespace {

// Target ABI: raw receiver -> result bits. Distinct ids per target let the
// tests observe WHICH marshalling executed.
uint64_t target_a(void*) noexcept { return 0xAA; }
uint64_t target_b(void*) noexcept { return 0xBB; }
uint64_t target_c(void*) noexcept { return 0xCC; }
uint64_t target_d(void*) noexcept { return 0xDD; }
uint64_t target_e(void*) noexcept { return 0xEE; }
uint64_t target_f(void*) noexcept { return 0xFF; }
uint64_t target_g(void*) noexcept { return 0x77; }

// Fake receiver: the trampoline reads header.klass at [rsi+0].
struct FakeObj {
    void* klass;
    uint64_t pad = 0;
};

// Fake klasses: aligned, distinct, non-zero (real Klass* stand-ins).
void* klass_a() { static int pad_a[4]; return pad_a; }
void* klass_b() { static int pad_b[4]; return pad_b; }
void* klass_c() { static int pad_c[4]; return pad_c; }
void* klass_d() { static int pad_d[4]; return pad_d; }
void* klass_e() { static int pad_e[4]; return pad_e; }
void* klass_f() { static int pad_f[4]; return pad_f; }
void* klass_g() { static int pad_g[4]; return pad_g; }

// Runtime target resolver: klass -> native target (the J4 dispatch seam).
struct ResolverState {
    uint64_t klass_ptrs[8] = {};
    uint64_t per_klass[8] = {};
};
uint64_t resolve_by_klass(void* user, uint32_t method_id,
                          uint32_t instruction_index, void* receiver,
                          bool* ok) noexcept {
    auto* st = static_cast<ResolverState*>(user);
    (void)method_id;
    (void)instruction_index;
    const uint64_t klass = *static_cast<uint64_t*>(receiver);
    for (int i = 0; i < 8; ++i) {
        if (st->klass_ptrs[i] != 0 && st->klass_ptrs[i] == klass) {
            *ok = true;
            return st->per_klass[i];
        }
    }
    *ok = false;
    return 0;
}

uint64_t ptr_bits(const void* p) noexcept {
    return reinterpret_cast<uint64_t>(p);
}

template <typename T>
uint64_t fn_bits(T fn) noexcept {
    return reinterpret_cast<uint64_t>(fn);
}

// The trampoline ABI: rdi = scratch (site handle), rsi = receiver.
using TrampolineFn = uint64_t (*)(void* scratch, void* receiver) noexcept;

uint64_t call_trampoline(void* entry, FakeObj& obj) {
    auto fn = reinterpret_cast<TrampolineFn>(entry);
    return fn(nullptr, &obj);
}

}  // namespace

VORTEX_TEST(ldpt_arena_wx_lifecycle) {
    auto arena = infra::PatchArena::allocate(4096);
    VORTEX_EXPECT(arena.has_value());
    if (!arena) return;
    VORTEX_EXPECT(!arena->published());

    const uint8_t bytes[8] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    VORTEX_EXPECT(arena->write(bytes, 0).has_value());
    // Carve is 16-byte aligned and bumps used.
    auto off = arena->carve(8);
    VORTEX_EXPECT(off.has_value());
    if (off) VORTEX_EXPECT_EQ(*off % 16, static_cast<size_t>(0));

    VORTEX_EXPECT(arena->publish().has_value());
    VORTEX_EXPECT(arena->published());
    // Emission writes are refused after publish (W^X law).
    VORTEX_EXPECT(!arena->write(bytes, 16).has_value());
    VORTEX_EXPECT(!arena->carve(8).has_value());
    // Patching requires a session.
    VORTEX_EXPECT(!arena->patch(bytes, 0).has_value());
    VORTEX_EXPECT(arena->begin_session().has_value());
    VORTEX_EXPECT(arena->in_session());
    VORTEX_EXPECT(!arena->begin_session().has_value());  // no nesting
    VORTEX_EXPECT(arena->patch(bytes, 0).has_value());
    VORTEX_EXPECT(arena->end_session().has_value());
    VORTEX_EXPECT(!arena->in_session());
}

VORTEX_TEST(ldpt_skeleton_primary_hit) {
    LdptManager mgr;
    auto site = mgr.emit_skeleton(1, 0, ptr_bits(klass_a()),
                                  fn_bits(&target_a));
    VORTEX_EXPECT(site.has_value());
    if (!site) return;
    VORTEX_EXPECT(mgr.publish().has_value());
    VORTEX_EXPECT_EQ((*site)->state, TrampolineState::Mono);
    VORTEX_EXPECT_EQ(mgr.stats().skeletons_emitted, static_cast<uint64_t>(1));

    FakeObj obj{klass_a()};
    VORTEX_EXPECT_EQ(call_trampoline(mgr.entry_of(**site), obj),
                     static_cast<uint64_t>(0xAA));
}

VORTEX_TEST(ldpt_first_miss_patches_mono) {
    infra::HandshakeManager handshake;
    LdptManager mgr({}, &handshake, nullptr);
    ResolverState rs;
    rs.klass_ptrs[1] = ptr_bits(klass_b());
    rs.per_klass[1] = fn_bits(&target_b);
    mgr.set_target_resolver(&resolve_by_klass, &rs);

    auto site = mgr.emit_skeleton(1, 0, ptr_bits(klass_a()),
                                  fn_bits(&target_a));
    VORTEX_EXPECT(site.has_value());
    if (!site) return;
    VORTEX_EXPECT(mgr.publish().has_value());
    void* entry = mgr.entry_of(**site);

    // Primary type: direct dispatch, no resolver involvement.
    FakeObj a{klass_a()};
    VORTEX_EXPECT_EQ(call_trampoline(entry, a), static_cast<uint64_t>(0xAA));
    VORTEX_EXPECT_EQ(mgr.stats().misses_resolved, static_cast<uint64_t>(0));

    // First miss: the resolver runs, the hole is patched, the call lands.
    FakeObj b{klass_b()};
    VORTEX_EXPECT_EQ(call_trampoline(entry, b), static_cast<uint64_t>(0xBB));
    VORTEX_EXPECT_EQ(mgr.stats().misses_resolved, static_cast<uint64_t>(1));
    VORTEX_EXPECT_EQ((*site)->state, TrampolineState::Poly);
    VORTEX_EXPECT((*site)->lookup(ptr_bits(klass_b())) != nullptr);

    // Steady state: the patched stub dispatches directly (same result).
    VORTEX_EXPECT_EQ(call_trampoline(entry, b), static_cast<uint64_t>(0xBB));
    VORTEX_EXPECT_EQ(mgr.stats().misses_resolved, static_cast<uint64_t>(1));
    // The handshake protocol COMPLETED around the patch: request raised
    // pending, acknowledge cleared it (M1 stub records the ordering; the
    // real M2 scheduler binds cooperative yields here).
    VORTEX_EXPECT(!handshake.pending(0));
}

VORTEX_TEST(ldpt_poly_escalation) {
    LdptManager mgr;
    ResolverState rs;
    void* klasses[5] = {klass_b(), klass_c(), klass_d(), klass_e()};
    uint64_t targets[5] = {fn_bits(&target_b), fn_bits(&target_c),
                           fn_bits(&target_d), fn_bits(&target_e)};
    for (int i = 0; i < 4; ++i) {
        rs.klass_ptrs[i + 1] = ptr_bits(klasses[i]);
        rs.per_klass[i + 1] = targets[i];
    }
    mgr.set_target_resolver(&resolve_by_klass, &rs);

    auto site = mgr.emit_skeleton(1, 0, ptr_bits(klass_a()),
                                  fn_bits(&target_a));
    VORTEX_EXPECT(site.has_value());
    if (!site) return;
    VORTEX_EXPECT(mgr.publish().has_value());
    void* entry = mgr.entry_of(**site);

    // Drive types b..e through the resolver: the poly chain caps at 4
    // entries (primary + 3 alternatives, ugb::kIcPolyCapacity); the 4th
    // alternative escalates to Mega.
    FakeObj a{klass_a()};
    VORTEX_EXPECT_EQ(call_trampoline(entry, a), 0xAA);
    FakeObj objs[4] = {{klass_b()}, {klass_c()}, {klass_d()}, {klass_e()}};
    const uint64_t want[4] = {0xBB, 0xCC, 0xDD, 0xEE};
    for (int i = 0; i < 4; ++i) {
        VORTEX_EXPECT_EQ(call_trampoline(entry, objs[i]), want[i]);
    }
    VORTEX_EXPECT_EQ((*site)->poly_count, static_cast<uint8_t>(4));
    VORTEX_EXPECT_EQ((*site)->state, TrampolineState::Mega);
    // Every type still dispatches correctly through the mega row.
    VORTEX_EXPECT_EQ(call_trampoline(entry, a), 0xAA);
    for (int i = 0; i < 4; ++i) {
        VORTEX_EXPECT_EQ(call_trampoline(entry, objs[i]), want[i]);
    }
}

VORTEX_TEST(ldpt_mega_swizzle_needs_no_session) {
    LdptManager mgr;
    ResolverState rs;
    void* klasses[6] = {klass_b(), klass_c(), klass_d(), klass_e(), klass_f(),
                        klass_g()};
    uint64_t targets[6] = {fn_bits(&target_b), fn_bits(&target_c),
                           fn_bits(&target_d), fn_bits(&target_e),
                           fn_bits(&target_f), fn_bits(&target_g)};
    for (int i = 0; i < 6; ++i) {
        rs.klass_ptrs[i + 1] = ptr_bits(klasses[i]);
        rs.per_klass[i + 1] = targets[i];
    }
    mgr.set_target_resolver(&resolve_by_klass, &rs);
    auto site = mgr.emit_skeleton(1, 0, ptr_bits(klass_a()),
                                  fn_bits(&target_a));
    VORTEX_EXPECT(site.has_value());
    if (!site) return;
    VORTEX_EXPECT(mgr.publish().has_value());
    void* entry = mgr.entry_of(**site);

    FakeObj a{klass_a()};
    FakeObj objs[6] = {{klass_b()}, {klass_c()}, {klass_d()},
                       {klass_e()}, {klass_f()}, {klass_g()}};
    const uint64_t want[6] = {0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x77};
    for (int i = 0; i < 6; ++i) {
        VORTEX_EXPECT_EQ(call_trampoline(entry, objs[i]), want[i]);
    }
    // All dispatches still correct after full escalation; the mega row
    // carries every type and the arena needed no further code patches for
    // types f/g (data-only swizzle).
    VORTEX_EXPECT_EQ(call_trampoline(entry, a), 0xAA);
    for (int i = 0; i < 6; ++i) {
        VORTEX_EXPECT_EQ(call_trampoline(entry, objs[i]), want[i]);
    }
    const auto* row = mgr.mega_row_for((*site)->handle);
    VORTEX_EXPECT(row != nullptr);
    if (row) {
        VORTEX_EXPECT_EQ(row->count, static_cast<uint32_t>(7));  // a + 6
        VORTEX_EXPECT(row->lookup(ptr_bits(klass_g())) != nullptr);
    }
}

VORTEX_TEST(ldpt_invalidation_degrades_to_resolver) {
    LdptManager mgr;
    ResolverState rs;
    rs.klass_ptrs[1] = ptr_bits(klass_b());
    rs.per_klass[1] = fn_bits(&target_b);
    mgr.set_target_resolver(&resolve_by_klass, &rs);
    auto site = mgr.emit_skeleton(1, 0, ptr_bits(klass_a()),
                                  fn_bits(&target_a));
    VORTEX_EXPECT(site.has_value());
    if (!site) return;
    VORTEX_EXPECT(mgr.publish().has_value());
    void* entry = mgr.entry_of(**site);

    FakeObj b{klass_b()};
    VORTEX_EXPECT_EQ(call_trampoline(entry, b), 0xBB);
    VORTEX_EXPECT_EQ((*site)->state, TrampolineState::Poly);

    // Dependency event: the target went away — revert the hole.
    auto inv = mgr.invalidate((*site)->site_id);
    VORTEX_EXPECT(inv.has_value());
    VORTEX_EXPECT_EQ((*site)->state, TrampolineState::Resolver);
    VORTEX_EXPECT_EQ(mgr.stats().invalidations, static_cast<uint64_t>(1));

    // The site still WORKS: the resolver re-resolves and re-patches.
    FakeObj a{klass_a()};
    VORTEX_EXPECT_EQ(call_trampoline(entry, a), 0xAA);
    VORTEX_EXPECT_EQ(call_trampoline(entry, b), 0xBB);
}

VORTEX_TEST(ldpt_dependency_batch_invalidation) {
    infra::DependencyGraph deps;
    LdptManager mgr({}, nullptr, &deps);
    auto site = mgr.emit_skeleton(42, 3, ptr_bits(klass_a()),
                                  fn_bits(&target_a));
    VORTEX_EXPECT(site.has_value());
    if (!site) return;
    VORTEX_EXPECT(mgr.publish().has_value());
    VORTEX_EXPECT_EQ(deps.edge_count(), static_cast<size_t>(1));

    // Unloading method 42 invalidates the registered assumption.
    auto batch = deps.invalidate_method(42);
    VORTEX_EXPECT_EQ(batch.immediate.size(), static_cast<size_t>(1));
    VORTEX_EXPECT(mgr.invalidate_batch(batch).has_value());
    VORTEX_EXPECT_EQ((*site)->state, TrampolineState::Resolver);
}

VORTEX_TEST(ldpt_unresolved_target_returns_zero) {
    LdptManager mgr;  // no target resolver installed
    auto site = mgr.emit_skeleton(1, 0, ptr_bits(klass_a()),
                                  fn_bits(&target_a));
    VORTEX_EXPECT(site.has_value());
    if (!site) return;
    VORTEX_EXPECT(mgr.publish().has_value());
    FakeObj b{klass_b()};
    // Resolver missing -> clean miss (0), no patch, site state unchanged.
    VORTEX_EXPECT_EQ(call_trampoline(mgr.entry_of(**site), b),
                     static_cast<uint64_t>(0));
    VORTEX_EXPECT_EQ(mgr.stats().misses_resolved, static_cast<uint64_t>(0));
}

VORTEX_TEST(ldpt_golden_skeleton_bytes) {
    LdptManager mgr;
    auto site = mgr.emit_skeleton(1, 0, 0x1234, fn_bits(&target_a));
    VORTEX_EXPECT(site.has_value());
    if (!site) return;
    const uint8_t* code = static_cast<const uint8_t*>(mgr.entry_of(**site));
    // Primary klass imm64 at +2.
    uint64_t klass = 0;
    std::memcpy(&klass, code + 2, 8);
    VORTEX_EXPECT_EQ(klass, static_cast<uint64_t>(0x1234));
    // cmp rax, [rsi]: 48 3b 06 (at +10, after the 10-byte imm64 mov)
    VORTEX_EXPECT_EQ(code[10], 0x48);
    VORTEX_EXPECT_EQ(code[11], 0x3B);
    VORTEX_EXPECT_EQ(code[12], 0x06);
    // jne rel32 to the hole: 0F 85 (at +13)
    VORTEX_EXPECT_EQ(code[13], 0x0F);
    VORTEX_EXPECT_EQ(code[14], 0x85);
    // mov rdi, rsi: 48 89 f7
    VORTEX_EXPECT_EQ(code[19], 0x48);
    VORTEX_EXPECT_EQ(code[20], 0x89);
    VORTEX_EXPECT_EQ(code[21], 0xF7);
    // mov rax, imm64 (target): 48 b8
    VORTEX_EXPECT_EQ(code[22], 0x48);
    VORTEX_EXPECT_EQ(code[23], 0xB8);
    // call rax: ff d0 ; ret: c3
    VORTEX_EXPECT_EQ(code[32], 0xFF);
    VORTEX_EXPECT_EQ(code[33], 0xD0);
    VORTEX_EXPECT_EQ(code[34], 0xC3);
    // Hole: mov rdi, imm32sx (48 c7 c7 ...) at the hole offset.
    const size_t hole =
        (*site)->hole_offset - (*site)->trampoline_offset;
    VORTEX_EXPECT_EQ(code[hole], 0x48);
    VORTEX_EXPECT_EQ(code[hole + 1], 0xC7);
    VORTEX_EXPECT_EQ(code[hole + 2], 0xC7);
}
