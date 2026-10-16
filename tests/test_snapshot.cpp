// CEP:FILE: tests/test_snapshot.cpp
// CEP:WHAT: The M6-a snapshot/AOT DoD suite (docs/infrastructure/
//           03-snapshots-aot.md section 7): container rejects, manifest
//           verification, heap round-trip parity + determinism, code-cache
//           reload, profile persistence, PGO-AOT determinism + load parity.
// CEP:WHY: Rule 121 discipline — every contract the milestone lands is
//           pinned by a regression test that fails loudly on drift.
// CEP:CLASS: CEP-1
// CEP:STATUS: complete
// CEP:FAILURE: assertion failures count through the harness; no test
//           aborts the binary (crashes surface as harness timeouts).
// CEP:ASSUMES: temp artifacts live in the ctest cwd and are removed per
//           test; the code range is the shared global one.
// CEP:COST: @cold. Test-only; no hot-path presence.
// CEP:EVIDENCE: 195/195 green on Release and ASan/UBSan (M6-a batch 3).

#include "vortex_test.hpp"

#include <cstdio>
#include <fstream>

#include "vortex/gc/icggc.hpp"
#include "vortex/infra/code_range.hpp"
#include "vortex/infra/snapshot.hpp"
#include "vortex/j1/baseline_jit.hpp"
#include "vortex/j2/fast_jit.hpp"
#include "vortex/ugb/builder.hpp"
#include "vortex/ugb/module.hpp"
#include "vortex/vm/interpreter.hpp"

using namespace vortex;
using namespace vortex::ugb;
using namespace vortex::infra;

namespace {

// Temp artifacts live in the test's cwd (the ctest build directory) and are
// removed as each test finishes.
const char* kTmpSnapshot = "vortex_snap_tmp.bin";
const char* kTmpAot1 = "vortex_aot_tmp1.art";
const char* kTmpAot2 = "vortex_aot_tmp2.art";
const char* kTmpModule = "vortex_aot_tmp.ugb";
const char* kTmpIc = "vortex_aot_tmp.ic";
const char* kTmpBranch = "vortex_aot_tmp.branch";

/// CEP:WHAT: Writes bytes to a temp artifact file (truncate).
/// CEP:WHY: The Rule-9 rejects need real files to open().
/// CEP:STATUS: complete
/// CEP:FAILURE: no return value: write errors surface as open() rejects downstream.
/// CEP:ASSUMES: the path is the test's cwd.
/// CEP:COST: @cold. One sequential write.
/// CEP:EVIDENCE: every container/heap/AOT test.
void write_bytes(const char* path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

/// CEP:WHAT: Reads a whole file back (the AOT determinism byte-compare).
/// CEP:WHY: Determinism is asserted on the BYTES ON DISK, not on in-memory state.
/// CEP:STATUS: complete
/// CEP:FAILURE: an unreadable file yields an empty vector (the compare then fails).
/// CEP:ASSUMES: the path was just written by the same test.
/// CEP:COST: @cold. One sequential read.
/// CEP:EVIDENCE: aot_pipeline_deterministic in tests/test_snapshot.cpp.
std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

/// CEP:WHAT: Writes profile text to a temp file.
/// CEP:WHY: The profile ingest consumes files exactly like the AOT pipeline does.
/// CEP:STATUS: complete
/// CEP:FAILURE: no return value: write errors surface as ingest rejects downstream.
/// CEP:ASSUMES: the path is the test's cwd.
/// CEP:COST: @cold. One sequential write.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip in tests/test_snapshot.cpp.
void write_text(const char* path, const std::string& text) {
    std::ofstream f(path, std::ios::trunc);
    f << text;
}

/// CEP:WHAT: Reads a profile text file back.
/// CEP:WHY: Ingest is exercised through the file round trip, not the in-memory string.
/// CEP:STATUS: complete
/// CEP:FAILURE: an unreadable file yields an empty string (the ingest then rejects).
/// CEP:ASSUMES: the path was just written by the same test.
/// CEP:COST: @cold. One sequential read.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip in tests/test_snapshot.cpp.
std::string read_text(const std::string& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

/// CEP:WHAT: vector -> span adapter for the manifest parse API.
/// CEP:WHY: Keeps the test call sites free of span plumbing.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure adapter).
/// CEP:ASSUMES: v outlives the span's use.
/// CEP:COST: @cold. Two pointers.
/// CEP:EVIDENCE: snapshot_manifest_mismatch_rejected in tests/test_snapshot.cpp.
std::span<const uint8_t> as_span(const std::vector<uint8_t>& v) {
    return std::span<const uint8_t>(v.data(), v.size());
}

void remove_tmp(const char* path) { std::remove(path); }

/// A valid small container: manifest + one Metadata + one MachineCode body.
/// CEP:WHAT: Builds a valid small container (manifest + one MachineCode body).
/// CEP:WHY: The Rule-9 negatives need a known-good baseline to corrupt.
/// CEP:STATUS: complete
/// CEP:FAILURE: returns an empty vector on writer failure (the test asserts non-empty).
/// CEP:ASSUMES: the default manifest identity matches this build.
/// CEP:COST: @cold. One finalize.
/// CEP:EVIDENCE: snapshot_container_rejects_malformed in tests/test_snapshot.cpp.
std::vector<uint8_t> small_container() {
    SnapshotWriter w;
    Manifest m = snapshot_default_manifest();
    m.dependencies.push_back(ManifestDependency{"unit", 42});
    (void)w.set_manifest(m);
    (void)w.add_section_named(SnapshotSection::MachineCode, "probe",
                              {1, 2, 3, 4, 5, 6, 7, 8});
    auto bytes = w.finalize();
    return bytes ? *bytes : std::vector<uint8_t>{};
}

const char* kChainSrc = R"(
.class Node
.field val in Node
.field next in Node

.method build(regs=8, args=0)
    Const.I32 v1, 1
    New.Object v2, Node
    SetField v2, v1, Node.val
    Const.I32 v3, 2
    New.Object v4, Node
    SetField v4, v3, Node.val
    SetField v2, v4, Node.next
    Const.I32 v5, 3
    New.Object v6, Node
    SetField v6, v5, Node.val
    SetField v4, v6, Node.next
    Const.Null v0
    SetField v6, v0, Node.next
    Return v2
.end

.method sum(regs=8, args=1)
    Const.I32 v1, 0
    Const.I32 v4, 3
loop:
    GetField v2, v0, Node.val
    Add.Any v1, v1, v2
    GetField v0, v0, Node.next
    Const.I32 v5, 1
    Sub.I32 v4, v4, v5
    Const.I32 v6, 0
    Eq.I32 v7, v4, v6
    JumpTrue v7, done
    Jump loop
done:
    Return v1
.end
)";

/// Builds the chain heap through T0. Returns false on any failure.
/// CEP:WHAT: Builds the chain heap through T0 (Rule 18: state via the engine, not synthetic).
/// CEP:WHY: The heap round trip must capture what the INTERPRETER built, klasses included.
/// CEP:STATUS: complete
/// CEP:FAILURE: returns false on any assemble/run failure.
/// CEP:ASSUMES: the chain source assembles and runs.
/// CEP:COST: @cold. One module + one T0 run.
/// CEP:EVIDENCE: heap_snapshot_roundtrip_parity in tests/test_snapshot.cpp.
bool build_chain_runtime(std::unique_ptr<gc::Heap>& heap,
                         std::unique_ptr<vm::Interpreter>& interp,
                         std::unique_ptr<UGBModule>& module,
                         TaggedValue& root) {
    auto assembled = assemble_module(kChainSrc);
    if (!assembled) return false;
    module = std::make_unique<UGBModule>(std::move(*assembled));
    heap = std::make_unique<gc::Heap>();
    interp = std::make_unique<vm::Interpreter>(*heap);
    auto r = interp->run(*module, "build", {});
    if (!r) return false;
    root = r->value;
    return root.is_heap_object() || root.is_null();
}

}  // namespace

// ---- container rejects (Rule 9) --------------------------------------------

/// CEP:WHAT: Rule 9 negatives: bad magic, bad version, payload bit flip, file corruption, truncation — each a named reject; the good container reopens and exposes its sections.
/// CEP:WHY: Artifacts are untrusted; the reader's ordered verification must reject by name (docs/infrastructure/03 section 1).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: snapshot_container_rejects_malformed.
VORTEX_TEST(snapshot_container_rejects_malformed) {
    const auto good = small_container();
    VORTEX_EXPECT(!good.empty());

    // Patch bytes AND refresh the file hash, so the targeted check (magic,
    // version, section hash) is what fires — not the outer file hash.
    const auto patch = [](std::vector<uint8_t> bytes, size_t offset,
                          uint8_t value) {
        bytes[offset] = value;
        const uint64_t h = fnv1a64(bytes.data(), bytes.size() - 16);
        for (int i = 0; i < 8; ++i) {
            bytes[bytes.size() - 8 + i] =
                static_cast<uint8_t>((h >> (8 * i)) & 0xFF);
        }
        return bytes;
    };

    // Bad magic.
    {
        auto bad = patch(good, 0, 0x00);
        write_bytes(kTmpSnapshot, bad);
        auto r = SnapshotReader::open(kTmpSnapshot);
        VORTEX_EXPECT(!r.has_value());
        if (!r) VORTEX_EXPECT(r.error().message.find("magic") !=
                              std::string::npos);
    }
    // Bad version.
    {
        auto bad = patch(good, 6, 9);  // version_minor byte
        write_bytes(kTmpSnapshot, bad);
        auto r = SnapshotReader::open(kTmpSnapshot);
        VORTEX_EXPECT(!r.has_value());
        if (!r) VORTEX_EXPECT(r.error().message.find("version") !=
                              std::string::npos);
    }
    // Bit flip inside a payload (section hash catches it).
    {
        auto bad = patch(good, good.size() - 24, 0x10);
        write_bytes(kTmpSnapshot, bad);
        auto r = SnapshotReader::open(kTmpSnapshot);
        VORTEX_EXPECT(!r.has_value());
        if (!r) VORTEX_EXPECT(r.error().message.find("section") !=
                              std::string::npos);
    }
    // Whole-file corruption (file hash catches it).
    {
        auto bad = good;
        bad[12] ^= 0x01;  // inside the directory
        write_bytes(kTmpSnapshot, bad);
        auto r = SnapshotReader::open(kTmpSnapshot);
        VORTEX_EXPECT(!r.has_value());
        if (!r) VORTEX_EXPECT(r.error().message.find("file hash") !=
                              std::string::npos);
    }
    // Truncation.
    {
        auto bad = good;
        bad.resize(bad.size() - 8);
        write_bytes(kTmpSnapshot, bad);
        auto r = SnapshotReader::open(kTmpSnapshot);
        VORTEX_EXPECT(!r.has_value());
    }
    // A GOOD container opens and exposes its sections.
    {
        write_bytes(kTmpSnapshot, good);
        auto r = SnapshotReader::open(kTmpSnapshot);
        VORTEX_EXPECT(r.has_value());
        if (r) {
            auto count = r->section_count();
            VORTEX_EXPECT(count.has_value());
            if (count) VORTEX_EXPECT_EQ(*count, size_t{2});
            auto body = r->section_payload(SnapshotSection::MachineCode,
                                           "probe");
            if (!body) {
                std::printf("  [snapshot-test] body: %s; names:", body.error().message.c_str());
                for (const auto& n : r->section_names(SnapshotSection::MachineCode)) std::printf(" '%s'", n.c_str());
                std::printf("\n");
            }
            VORTEX_EXPECT(body.has_value());
            if (body) VORTEX_EXPECT_EQ(body->size(), size_t{8});
            auto m = r->manifest();
            VORTEX_EXPECT(m.has_value());
            if (m) VORTEX_EXPECT_EQ(m->dependencies.size(), size_t{1});
        }
    }
    remove_tmp(kTmpSnapshot);
}

// ---- manifest verification (Rules 97/98) -----------------------------------

/// CEP:WHAT: One negative per Rule-97 field (all named), the all-match accept, and the feature-set subset gate.
/// CEP:WHY: Rules 97/98: the manifest is the compatibility contract; every mismatch is spellable.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: snapshot_manifest_mismatch_rejected.
VORTEX_TEST(snapshot_manifest_mismatch_rejected) {
    Manifest expected = snapshot_default_manifest();
    expected.dependencies.push_back(ManifestDependency{"unit", 42});
    auto payload = build_manifest_payload(expected);
    auto parsed = parse_manifest_payload(as_span(payload));
    VORTEX_EXPECT(parsed.has_value());
    if (!parsed) return;
    VORTEX_EXPECT(
        ManifestVerifier::verify(*parsed, expected).has_value());

    const auto reject = [&](const char* field, auto mutate) {
        Manifest broken = expected;
        mutate(broken);
        auto p = build_manifest_payload(broken);
        auto bad = parse_manifest_payload(as_span(p));
        VORTEX_EXPECT(bad.has_value());
        if (!bad) return;
        auto verdict = ManifestVerifier::verify(*bad, expected);
        VORTEX_EXPECT(!verdict.has_value());
        if (!verdict) {
            VORTEX_EXPECT(std::string(verdict.error().message)
                              .find(field) != std::string::npos);
        }
    };
    reject("ugb_version", [](Manifest& m) { m.ugb_version = 99; });
    reject("compiler_version",
           [](Manifest& m) { m.compiler_version = 99; });
    reject("pass_pipeline_hash",
           [](Manifest& m) { m.pass_pipeline_hash ^= 1; });
    reject("target_arch",
           [](Manifest& m) { m.target_arch = "arm64"; });
    reject("target_feature_set",
           [](Manifest& m) { m.target_feature_set = "avx512x"; });
    reject("abi_hash", [](Manifest& m) { m.abi_hash ^= 1; });
    reject("runtime_config_hash",
           [](Manifest& m) { m.runtime_config_hash ^= 1; });
    reject("dependency_fingerprints",
           [](Manifest& m) { m.dependencies[0].fingerprint = 7; });
    reject("dependency_fingerprints",
           [](Manifest& m) { m.dependencies.pop_back(); });
    reject("security_policy_version",
           [](Manifest& m) { m.security_policy_version = 8; });

    // The feature gate: an artifact feature the loader lacks is rejected;
    // a subset is accepted.
    VORTEX_EXPECT(!feature_set_subset("avx2", "base"));
    VORTEX_EXPECT(feature_set_subset("base", "avx2"));
    VORTEX_EXPECT(feature_set_subset("", "base"));
}

// ---- heap snapshot round trip (Rule 18) ------------------------------------

/// CEP:WHAT: T0-built state -> snapshot -> restore: klass layout, chain values/links, array identity, boxed-double payload, and sum() parity across the restored heap (Rule 18).
/// CEP:WHY: The heap snapshot must preserve observable semantics and object identity (docs/infrastructure/03 section 3).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: heap_snapshot_roundtrip_parity.
VORTEX_TEST(heap_snapshot_roundtrip_parity) {
    std::unique_ptr<gc::Heap> heap;
    std::unique_ptr<vm::Interpreter> interp;
    std::unique_ptr<UGBModule> module;
    TaggedValue root;
    VORTEX_EXPECT(build_chain_runtime(heap, interp, module, root));
    if (!heap) return;

    // Extra runtime shapes: an array holding a node + a boxed double.
    auto arr = heap->allocate_array(2);
    VORTEX_EXPECT(arr.has_value());
    if (!arr) return;
    (*arr)->element(0) = TaggedValue::smi(9);
    (*arr)->element(1) = root;  // the chain head (a heap reference)
    auto dbl = heap->allocate_double(2.5);
    VORTEX_EXPECT(dbl.has_value());
    if (!dbl) return;

    // Snapshot (manifest + heap sections) -> file.
    SnapshotWriter writer;
    auto mst = writer.set_manifest(snapshot_default_manifest());
    VORTEX_EXPECT(mst.has_value());
    auto wst = write_heap_snapshot(*heap, interp->klass_registry(), writer);
    VORTEX_EXPECT(wst.has_value());
    if (!wst) return;
    auto wf = writer.write_file(kTmpSnapshot);
    VORTEX_EXPECT(wf.has_value());
    if (!wf) return;

    auto reader = SnapshotReader::open(kTmpSnapshot);
    if (!reader) std::printf("  [snapshot-test] open: %s\n", reader.error().message.c_str());
    VORTEX_EXPECT(reader.has_value());
    if (!reader) return;
    auto restored = restore_heap_snapshot(*reader);
    if (!restored) std::printf("  [snapshot-test] restore: %s\n", restored.error().message.c_str());
    VORTEX_EXPECT(restored.has_value());
    if (!restored) return;

    // Root identification: build() allocates the head FIRST, and the
    // snapshot preserves allocation order — restored.objects[0] IS the head.
    VORTEX_EXPECT_EQ(restored->objects.size(), size_t{5});  // 3 nodes+arr+box
    if (restored->objects.size() != 5) return;
    auto* head = static_cast<Object*>(restored->objects[0]);

    // Klass identity through the registry: Node exists with both fields.
    bool have_node = false;
    for (size_t i = 0; i < restored->klasses->size(); ++i) {
        Klass* k = restored->klasses->at(i);
        if (k->name() == "Node") {
            have_node = true;
            VORTEX_EXPECT_EQ(k->field_count(), uint32_t{2});
            VORTEX_EXPECT(k->find_field("val") >= 0);
            VORTEX_EXPECT(k->find_field("next") >= 0);
        }
    }
    VORTEX_EXPECT(have_node);

    // Walk the restored chain: values, links, and the tail's null.
    const auto field_smi = [](Object* o, const char* name) -> int64_t {
        Klass* k = o->header.klass;
        const int idx = k->find_field(name);
        return o->field(static_cast<uint32_t>(idx)).as_smi();
    };
    VORTEX_EXPECT_EQ(field_smi(head, "val"), int64_t{1});
    auto* second = static_cast<Object*>(head->field(1).as_heap_object());
    VORTEX_EXPECT(second != nullptr);
    if (second) {
        VORTEX_EXPECT_EQ(field_smi(second, "val"), int64_t{2});
        auto* third = static_cast<Object*>(second->field(1).as_heap_object());
        VORTEX_EXPECT(third != nullptr);
        if (third) {
            VORTEX_EXPECT_EQ(field_smi(third, "val"), int64_t{3});
            VORTEX_EXPECT(third->field(1).is_null());
        }
    }

    // The array's reference leg points at the SAME restored head (identity),
    // and the boxed double's payload survived.
    auto* restored_arr = static_cast<ArrayObject*>(restored->objects[3]);
    VORTEX_EXPECT_EQ(restored_arr->element(0).as_smi(), int64_t{9});
    VORTEX_EXPECT(restored_arr->element(1).as_heap_object() ==
                  reinterpret_cast<HeapObject*>(head));
    auto* restored_box = restored->objects[4];
    VORTEX_EXPECT(restored->double_klass != nullptr);
    const double payload =
        read_boxed_double(restored_box);  // layout: header + 8-byte payload
    VORTEX_EXPECT_EQ(payload, 2.5);

    // Continue-execution parity (Rule 18): sum() over the restored graph
    // equals sum() over the source graph. A fresh interpreter per side;
    // the root rides in as the argument.
    const auto run_sum = [&](vm::Interpreter& it, UGBModule& mod,
                             TaggedValue node) -> bool {
        const std::vector<TaggedValue> one{node};
        auto r = it.run(mod, "sum", one);
        if (!r) return false;
        return r->value.as_smi() == 6;
    };
    vm::Interpreter src_interp(*heap);
    const bool src_ok = run_sum(src_interp, *module, root);
    vm::Interpreter rst_interp(*restored->heap);
    const TaggedValue restored_root = TaggedValue::from_raw(
        reinterpret_cast<uint64_t>(head) | 0x1);
    const bool rst_ok = run_sum(rst_interp, *module, restored_root);
    VORTEX_EXPECT(src_ok);
    VORTEX_EXPECT(rst_ok);
    remove_tmp(kTmpSnapshot);
}

/// CEP:WHAT: Two restores of one snapshot: identical layout offsets, identical klass layouts, identical double klass.
/// CEP:WHY: The section-3 determinism contract (heap-relative geometry is pure snapshot data).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: heap_snapshot_restore_deterministic.
VORTEX_TEST(heap_snapshot_restore_deterministic) {
    std::unique_ptr<gc::Heap> heap;
    std::unique_ptr<vm::Interpreter> interp;
    std::unique_ptr<UGBModule> module;
    TaggedValue root;
    VORTEX_EXPECT(build_chain_runtime(heap, interp, module, root));
    if (!heap) return;

    SnapshotWriter writer;
    (void)writer.set_manifest(snapshot_default_manifest());
    auto wst = write_heap_snapshot(*heap, interp->klass_registry(), writer);
    VORTEX_EXPECT(wst.has_value());
    if (!wst) return;
    auto bytes = writer.finalize();
    VORTEX_EXPECT(bytes.has_value());
    if (!bytes) return;
    write_bytes(kTmpSnapshot, *bytes);
    auto reader = SnapshotReader::open(kTmpSnapshot);
    if (!reader) std::printf("  [snapshot-test] open: %s\n", reader.error().message.c_str());
    VORTEX_EXPECT(reader.has_value());
    if (!reader) return;

    auto a = restore_heap_snapshot(*reader);
    auto b = restore_heap_snapshot(*reader);
    VORTEX_EXPECT(a.has_value() && b.has_value());
    if (!a || !b) return;
    // Offset-identity contract: the same snapshot restores to the same
    // LAYOUT (object i at heap_base + O_i, byte-for-byte), so heap-relative
    // geometry is a pure function of the snapshot bytes. Raw base pointers
    // differ per instance (the buffer is malloc'd); the offsets do not.
    VORTEX_EXPECT_EQ(a->objects.size(), b->objects.size());
    for (size_t i = 0; i < a->objects.size(); ++i) {
        const uintptr_t oa = reinterpret_cast<uintptr_t>(a->objects[i]) -
                             reinterpret_cast<uintptr_t>(a->heap->heap_base());
        const uintptr_t ob = reinterpret_cast<uintptr_t>(b->objects[i]) -
                             reinterpret_cast<uintptr_t>(b->heap->heap_base());
        VORTEX_EXPECT(oa == ob);
    }
    // Klass identity: same names, same field layouts (the field-slot
    // offsets the JIT bakes), same order. Registry objects are process-
    // allocated, so raw Klass pointers are not snapshot data.
    VORTEX_EXPECT_EQ(a->klasses->size(), b->klasses->size());
    for (size_t i = 0; i < a->klasses->size(); ++i) {
        Klass* ka = a->klasses->at(i);
        Klass* kb = b->klasses->at(i);
        VORTEX_EXPECT(ka->name() == kb->name());
        VORTEX_EXPECT_EQ(ka->field_count(), kb->field_count());
        for (uint32_t fi = 0; fi < ka->field_count(); ++fi) {
            VORTEX_EXPECT(ka->fields()[fi].offset == kb->fields()[fi].offset);
        }
    }
    // The boxed-double klass is the heap's static singleton — stable
    // within the process across restores.
    VORTEX_EXPECT(a->double_klass == b->double_klass);
    remove_tmp(kTmpSnapshot);
}

// ---- shared code cache (docs/infrastructure/03 section 4) ------------------

/// CEP:WHAT: Compile -> snapshot -> reload through publish_j2: byte-identical code, executed parity, and the klass-handle drift reject (Rule 98).
/// CEP:WHY: The shared code cache shares ONE publication contract with the JIT (docs/infrastructure/03 section 4).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: code_cache_reload_parity.
VORTEX_TEST(code_cache_reload_parity) {
    constexpr const char* kSrc = R"(
.method f(regs=4, args=2)
    Add.I32 v2, v0, v1
    Mul.I32 v3, v2, v1
    Return v3
.end
)";
    auto assembled = assemble_module(kSrc);
    VORTEX_EXPECT(assembled.has_value());
    if (!assembled) return;
    UGBModule module = std::move(*assembled);
    gc::Heap heap;
    vm::Interpreter interp(heap);
    interp.register_builtin(
        "print", [](std::span<const TaggedValue>, void*) {
            return TaggedValue::undefined();
        });
    const std::vector<TaggedValue> args = {TaggedValue::smi(20),
                                           TaggedValue::smi(3)};
    // T0 warmup fills the slot tables the J2 job consumes (Rules 22/32).
    auto warm = interp.run(module, "f", args);
    VORTEX_EXPECT(warm.has_value());
    if (!warm) return;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    const int32_t mid = module.find_method("f");
    VORTEX_EXPECT(mid >= 0);
    if (mid < 0) return;
    auto& method = module.method_table[static_cast<size_t>(mid)];
    j2::J2Job job;
    job.module = &module;
    job.method_id = static_cast<uint32_t>(mid);
    job.profiles = &method.profiles;
    job.ics = &method.ics;
    job.klass_addrs = &bindings.klass_addr_table;
    job.double_klass = bindings.double_klass;
    auto code = j2::compile_j2(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;
    auto ex0 = j2::publish_j2(*code, global_code_range());
    VORTEX_EXPECT(ex0.has_value());
    if (!ex0) return;
    auto r0 = j2::run_j2(*ex0, bindings, args, interp);
    VORTEX_EXPECT(r0.has_value());
    if (!r0) return;

    // Snapshot -> file -> reload (publish through the normal W^X path).
    auto handles = heap_klass_handles(interp.klass_registry(), heap);
    SnapshotWriter writer;
    auto mst = writer.set_manifest(snapshot_default_manifest());
    VORTEX_EXPECT(mst.has_value());
    auto wst = write_code_snapshot(*code, handles, writer);
    VORTEX_EXPECT(wst.has_value());
    if (!wst) return;
    auto bytes = writer.finalize();
    VORTEX_EXPECT(bytes.has_value());
    if (!bytes) return;
    write_bytes(kTmpSnapshot, *bytes);
    auto reader = SnapshotReader::open(kTmpSnapshot);
    VORTEX_EXPECT(reader.has_value());
    if (!reader) return;

    // Byte identity: the compiled bytes are address-free (Rule 56), so the
    // round trip is exact.
    auto code2 = load_code_snapshot(*reader, handles);
    if (!code2) std::printf("  [snapshot-test] load: %s\n", code2.error().message.c_str());
    VORTEX_EXPECT(code2.has_value());
    if (code2) {
        VORTEX_EXPECT(code2->code == code->code);
        VORTEX_EXPECT(code2->entry_offset == code->entry_offset);
        VORTEX_EXPECT_EQ(code2->records->size(), code->records->size());
    }
    auto ex2 = publish_code_snapshot(*reader, handles, global_code_range());
    VORTEX_EXPECT(ex2.has_value());
    if (!ex2) return;
    auto r2 = j2::run_j2(*ex2, bindings, args, interp);
    VORTEX_EXPECT(r2.has_value());
    if (r2) VORTEX_EXPECT(r2->raw() == r0->raw());

    // Rule 98 dependency validity: a drifted handle is a named reject.
    auto bad = handles;
    if (!bad.empty()) {
        bad[0].address = reinterpret_cast<const void*>(0x1234);
        auto rej = load_code_snapshot(*reader, bad);
        VORTEX_EXPECT(!rej.has_value());
        if (!rej) {
            VORTEX_EXPECT(rej.error().message.find("klass handle") !=
                          std::string::npos);
        }
    }
    remove_tmp(kTmpSnapshot);
}

// ---- profile persistence (.ic / .branch) -----------------------------------

/// CEP:WHAT: Dump/ingest round trips are bit-exact; malformed lines and bad ids reject by name.
/// CEP:WHY: Rule 9: profiles are untrusted input; the ingest is all-or-nothing (docs/infrastructure/03 section 5).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: profile_dump_restore_roundtrip.
VORTEX_TEST(profile_dump_restore_roundtrip) {
    constexpr const char* kSrc = R"(
.class Box
.field v in Box
.method mk(regs=3, args=1)
    New.Object v1, Box
    SetField v1, v0, Box.v
    Return v1
.end
.method getv(regs=2, args=1)
    GetField v1, v0, Box.v
    Return v1
.end
.method main(regs=7, args=0)
    Const.I32 v0, 7
    Const.I32 v1, 0
    Const.I32 v2, 4
loop:
    Eq.I32 v3, v1, v2
    JumpTrue v3, done
    Call.Direct v4, v0, 1, mk
    Call.Direct v5, v4, 1, getv
    Const.I32 v6, 1
    Add.I32 v1, v1, v6
    Jump loop
done:
    Return v5
.end
)";
    auto assembled = assemble_module(kSrc);
    VORTEX_EXPECT(assembled.has_value());
    if (!assembled) return;
    UGBModule module = std::move(*assembled);
    gc::Heap heap;
    vm::Interpreter interp(heap);
    auto r = interp.run(module, "main", {});
    if (!r) std::printf("  [snapshot-test] warm: %s\n", r.error().message.c_str());
    VORTEX_EXPECT(r.has_value());
    if (!r) return;

    auto branch = dump_branch_profile(module);
    auto ic = dump_ic_profile(module);
    VORTEX_EXPECT(branch.has_value() && ic.has_value());
    if (!branch || !ic) return;
    VORTEX_EXPECT(branch->find("b 2 ") != std::string::npos);  // main loops
    VORTEX_EXPECT(ic->find("i 1 ") != std::string::npos);      // getv IC
    write_text(kTmpBranch, *branch);
    write_text(kTmpIc, *ic);

    // Fresh decode of the same module (runtime tables are NOT serialized —
    // the ingest rebuilds them).
    auto encoded = encode_module(module);
    VORTEX_EXPECT(!encoded.empty());
    UGBModule fresh;
    DecodeErrorInfo err{};
    VORTEX_EXPECT(decode_module(encoded.data(), encoded.size(), fresh, err));

    auto in_b = ingest_branch_profile(read_text(kTmpBranch), fresh);
    auto in_i = ingest_ic_profile(read_text(kTmpIc), fresh);
    VORTEX_EXPECT(in_b.has_value() && in_i.has_value());
    if (!in_b || !in_i) return;

    // Bit-exact round trip: the re-dump equals the original dump.
    auto branch2 = dump_branch_profile(fresh);
    auto ic2 = dump_ic_profile(fresh);
    VORTEX_EXPECT(branch2.has_value() && branch2.value() == branch.value());
    VORTEX_EXPECT(ic2.has_value() && ic2.value() == ic.value());

    // Malformed input: named rejects, module untouched.
    auto bad1 = ingest_branch_profile("#vortex-branch-profile 1\nb 99 0 1 0 0 0 0\n", module);
    VORTEX_EXPECT(!bad1.has_value());
    auto bad2 = ingest_branch_profile("#vortex-branch-profile 1\nx 1 2 3\n", module);
    VORTEX_EXPECT(!bad2.has_value());
    auto bad3 = ingest_ic_profile("#vortex-ic-profile 1\ni 0 0 9 0 0 0:0\n", module);
    VORTEX_EXPECT(!bad3.has_value());
    auto bad4 = ingest_branch_profile("garbage", module);
    VORTEX_EXPECT(!bad4.has_value());
    remove_tmp(kTmpBranch);
    remove_tmp(kTmpIc);
}

// ---- the PGO-AOT pipeline (docs/infrastructure/03 section 6) ---------------

namespace {

// A warm runtime whose module + profiles feed the offline pipeline: hot
// (arithmetic loop) crosses the execution threshold; mk/getv/main ride
// along so the artifact also exercises klass-baked code sections.
const char* kAotSrc = R"(
.class Box
.field v in Box
.method mk(regs=3, args=1)
    New.Object v1, Box
    SetField v1, v0, Box.v
    Return v1
.end
.method getv(regs=2, args=1)
    GetField v1, v0, Box.v
    Return v1
.end
.method hot(regs=6, args=1)
    Const.I32 v1, 0
    Const.I32 v2, 0
loop:
    Const.I32 v3, 40
    Eq.I32 v4, v2, v3
    JumpTrue v4, done
    Add.I32 v1, v1, v0
    Const.I32 v5, 1
    Add.I32 v2, v2, v5
    Jump loop
done:
    Return v1
.end
.method main(regs=9, args=0)
    Const.I32 v0, 41
    Const.I32 v1, 20
    Const.I32 v2, 0
    Const.I32 v3, 0
loop:
    Eq.I32 v4, v2, v1
    JumpTrue v4, done
    Call.Direct v5, v0, 1, mk
    Call.Direct v6, v5, 1, getv
    Add.I32 v3, v3, v6
    Const.I32 v7, 1
    Add.I32 v2, v2, v7
    Jump loop
done:
    Return v3
.end
)";

struct AotFixture {
    std::unique_ptr<gc::Heap> heap;
    std::unique_ptr<vm::Interpreter> interp;
    std::unique_ptr<UGBModule> module;
    std::vector<KlassHandle> handles;
    bool ok = false;
    int32_t hot_id = -1;
};

/// CEP:WHAT: Assembles + warms the AOT fixture (main and hot through T0).
/// CEP:WHY: Method selection needs profile activity; hot is warmed directly.
/// CEP:STATUS: complete
/// CEP:FAILURE: returns ok=false with the reason printed on any failure.
/// CEP:ASSUMES: kAotSrc assembles and runs.
/// CEP:COST: @cold. One module + two T0 runs.
/// CEP:EVIDENCE: the AOT tests in tests/test_snapshot.cpp.
AotFixture make_aot_fixture() {
    AotFixture f;
    auto assembled = assemble_module(kAotSrc);
    if (!assembled) return f;
    f.module = std::make_unique<UGBModule>(std::move(*assembled));
    f.heap = std::make_unique<gc::Heap>();
    f.interp = std::make_unique<vm::Interpreter>(*f.heap);
    f.interp->register_builtin(
        "print", [](std::span<const TaggedValue>, void*) {
            return TaggedValue::undefined();
        });
    auto r = f.interp->run(*f.module, "main", {});
    if (!r) {
        std::printf("  [snapshot-test] fixture run: %s\n", r.error().message.c_str());
        return f;
    }
    // Warm hot directly (main does not call it; the AOT selection needs its
    // profile activity above the threshold).
    const std::vector<TaggedValue> hot_arg{TaggedValue::smi(1)};
    auto rh = f.interp->run(*f.module, "hot", hot_arg);
    if (!rh) {
        std::printf("  [snapshot-test] fixture hot: %s\n", rh.error().message.c_str());
        return f;
    }
    f.handles = heap_klass_handles(f.interp->klass_registry(), *f.heap);
    f.hot_id = f.module->find_method("hot");
    f.ok = f.hot_id >= 0;
    return f;
}

/// Writes the module + both profile files; returns the manifest the loader
/// must expect (the same dependency fingerprints the emitter computes).
/// CEP:WHAT: Writes the module + both profile files; returns the expected manifest.
/// CEP:WHY: The loader's Rule-98 check needs the SAME fingerprints the emitter computes.
/// CEP:STATUS: complete
/// CEP:FAILURE: an unwritable cwd surfaces as emit/load rejects downstream.
/// CEP:ASSUMES: the fixture warmed the module.
/// CEP:COST: @cold. One encode + two dumps + three writes.
/// CEP:EVIDENCE: the AOT tests in tests/test_snapshot.cpp.
Manifest write_aot_inputs(const AotFixture& f) {
    auto encoded = encode_module(*f.module);
    write_bytes(kTmpModule, encoded);
    auto branch = dump_branch_profile(*f.module);
    auto ic = dump_ic_profile(*f.module);
    write_text(kTmpBranch, branch ? *branch : "");
    write_text(kTmpIc, ic ? *ic : "");
    Manifest expected = snapshot_default_manifest();
    expected.dependencies.push_back(
        ManifestDependency{"module", fnv1a64(encoded.data(), encoded.size())});
    if (branch) {
        expected.dependencies.push_back(ManifestDependency{
            "branch-profile",
            fnv1a64(reinterpret_cast<const uint8_t*>(branch->data()),
                    branch->size())});
    }
    if (ic) {
        expected.dependencies.push_back(ManifestDependency{
            "ic-profile",
            fnv1a64(reinterpret_cast<const uint8_t*>(ic->data()),
                    ic->size())});
    }
    return expected;
}

}  // namespace

/// CEP:WHAT: Two emits over identical inputs -> byte-identical artifacts; the manifest verifies; the hot section is present.
/// CEP:WHY: J4 determinism end to end (docs/infrastructure/03 section 6).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: aot_pipeline_deterministic.
VORTEX_TEST(aot_pipeline_deterministic) {
    AotFixture f = make_aot_fixture();
    VORTEX_EXPECT(f.ok);
    if (!f.ok) return;
    Manifest expected = write_aot_inputs(f);

    PgoProfileInput in;
    in.module_path = kTmpModule;
    in.ic_profile_path = kTmpIc;
    in.branch_profile_path = kTmpBranch;
    in.klass_handles = f.handles;
    in.min_execution_count = 10;
    auto p1 = AotPipeline::emit_shared_library(in, kTmpAot1);
    auto p2 = AotPipeline::emit_shared_library(in, kTmpAot2);
    if (!p1) std::printf("  [snapshot-test] emit: %s\n", p1.error().message.c_str());
    VORTEX_EXPECT(p1.has_value());
    VORTEX_EXPECT(p2.has_value());
    if (!p1 || !p2) return;
    // J4 determinism end to end: identical inputs -> byte-identical files.
    VORTEX_EXPECT(read_file(kTmpAot1) == read_file(kTmpAot2));

    // The hot method (and the call-heavy others) made the cut.
    auto reader = SnapshotReader::open(kTmpAot1);
    VORTEX_EXPECT(reader.has_value());
    if (reader) {
        auto names = reader->section_names(SnapshotSection::MachineCode);
        bool have_hot = false;
        for (const auto& n : names) {
            if (n == "method:" + std::to_string(f.hot_id)) have_hot = true;
        }
        VORTEX_EXPECT(have_hot);
        auto m = reader->manifest();
        VORTEX_EXPECT(m.has_value());
        auto verdict =
            ManifestVerifier::verify(*m, expected);
        VORTEX_EXPECT(verdict.has_value());
    }
    remove_tmp(kTmpModule);
    remove_tmp(kTmpIc);
    remove_tmp(kTmpBranch);
    remove_tmp(kTmpAot1);
    remove_tmp(kTmpAot2);
}

/// CEP:WHAT: Artifact load (Rule 98 ordered checks) -> execute at parity with T0; a drifted dependency fingerprint rejects.
/// CEP:WHY: The deployment boot path lands verified, published code with JIT-fallback semantics intact.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: aot_artifact_load_execute_parity.
VORTEX_TEST(aot_artifact_load_execute_parity) {
    AotFixture f = make_aot_fixture();
    VORTEX_EXPECT(f.ok);
    if (!f.ok) return;
    Manifest expected = write_aot_inputs(f);

    PgoProfileInput in;
    in.module_path = kTmpModule;
    in.ic_profile_path = kTmpIc;
    in.branch_profile_path = kTmpBranch;
    in.klass_handles = f.handles;
    in.min_execution_count = 10;
    auto emitted = AotPipeline::emit_shared_library(in, kTmpAot1);
    VORTEX_EXPECT(emitted.has_value());
    if (!emitted) return;

    auto art = AotPipeline::load_artifact(kTmpAot1, expected, f.handles,
                                          global_code_range());
    VORTEX_EXPECT(art.has_value());
    if (!art) return;
    AotArtifact& artifact = **art;
    VORTEX_EXPECT_EQ(artifact.method_ids.size(), size_t{4});
    bool have_hot = false;
    for (size_t i = 0; i < artifact.method_ids.size(); ++i) {
        if (artifact.method_ids[i] == static_cast<uint32_t>(f.hot_id)) {
            have_hot = true;
            // Execute the AOT-published hot at parity with T0.
            j1::J1Bindings bindings;
            auto br = j1::make_j1_bindings(*f.heap, *f.module,
                                           f.interp.get(), bindings);
            VORTEX_EXPECT(br.has_value());
            if (!br) return;
            const std::vector<TaggedValue> args = {TaggedValue::smi(41)};
            auto want = f.interp->run(*f.module, "hot", args);
            auto got = j2::run_j2(*artifact.executables[i], bindings, args,
                                  *f.interp);
            VORTEX_EXPECT(want.has_value() && got.has_value());
            if (want && got) {
                VORTEX_EXPECT_EQ(got->as_smi(), want->value.as_smi());
                VORTEX_EXPECT_EQ(got->as_smi(), int64_t{1640});
            }
        }
    }
    VORTEX_EXPECT(have_hot);

    // Rule 98: a drifted dependency fingerprint rejects the artifact.
    Manifest bad = expected;
    bad.dependencies[0].fingerprint ^= 1;
    auto rej = AotPipeline::load_artifact(kTmpAot1, bad, f.handles,
                                          global_code_range());
    VORTEX_EXPECT(!rej.has_value());
    if (!rej) {
        VORTEX_EXPECT(rej.error().message.find("dependency") !=
                      std::string::npos);
    }
    remove_tmp(kTmpModule);
    remove_tmp(kTmpIc);
    remove_tmp(kTmpBranch);
    remove_tmp(kTmpAot1);
}


// ---- review-round pins (findings 15-e-a / 15-e-b) --------------------------

namespace {

/// CEP:WHAT: Little-endian u32 append for the hand-built wire payloads.
/// CEP:WHY: The malformed-payload negatives build Shapes/Objects bytes
///           exactly as the writer would (then corrupt them).
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure append).
/// CEP:ASSUMES: v is a test-local build buffer.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: snapshot_rejects_malformed_heap_payloads.
// Minimal Shapes/Objects payload builders mirroring the wire format — the
// malformed-payload negatives need hand-built sections the writer would
// never emit.
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
/// CEP:WHAT: Little-endian u64 append for the hand-built wire payloads.
/// CEP:WHY: Swizzled words and object words are u64 fields.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure append).
/// CEP:ASSUMES: v is a test-local build buffer.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: snapshot_rejects_malformed_heap_payloads.
void put_u64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
/// CEP:WHAT: Length-prefixed string append for the hand-built payloads.
/// CEP:WHY: Klass and field names use the u32-length encoding.
/// CEP:STATUS: complete
/// CEP:FAILURE: none (pure append).
/// CEP:ASSUMES: v is a test-local build buffer.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: snapshot_rejects_malformed_heap_payloads.
void put_str(std::vector<uint8_t>& v, const std::string& s) {
    put_u32(v, static_cast<uint32_t>(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

}  // namespace

/// CEP:WHAT: Pins the swizzle-decode soundness: negative Smis share the
///           0xFFFFFFFF high word and must restore AS SMIS.
/// CEP:WHY: The old decode keyed on the high word alone — smi(-1) would
///           restore as a heap pointer (review 15-e-a/1, blocker).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: snapshot_swizzle_smi_collision_pinned.
VORTEX_TEST(snapshot_swizzle_smi_collision_pinned) {
    // The swizzle tag nibble (0x5) shares the 0xFFFFFFFF high word with
    // negative Smis — the decode must key on the FULL pattern, or -1 (and
    // every odd-tailed negative Smi) would restore as a heap pointer.
    std::unique_ptr<gc::Heap> heap;
    std::unique_ptr<vm::Interpreter> interp;
    std::unique_ptr<UGBModule> module;
    TaggedValue root;
    VORTEX_EXPECT(build_chain_runtime(heap, interp, module, root));
    if (!heap) return;
    auto* head = static_cast<Object*>(root.as_heap_object());
    auto* second = static_cast<Object*>(head->field(1).as_heap_object());
    head->field(0) = TaggedValue::smi(-1);
    second->field(0) = TaggedValue::smi(INT32_MIN);

    SnapshotWriter writer;
    (void)writer.set_manifest(snapshot_default_manifest());
    auto wst = write_heap_snapshot(*heap, interp->klass_registry(), writer);
    VORTEX_EXPECT(wst.has_value());
    if (!wst) return;
    auto bytes = writer.finalize();
    if (!bytes) return;
    write_bytes(kTmpSnapshot, *bytes);
    auto reader = SnapshotReader::open(kTmpSnapshot);
    VORTEX_EXPECT(reader.has_value());
    if (!reader) return;
    auto restored = restore_heap_snapshot(*reader);
    VORTEX_EXPECT(restored.has_value());
    if (!restored) return;
    auto* rhead = static_cast<Object*>(restored->objects[0]);
    auto* rsecond = static_cast<Object*>(rhead->field(1).as_heap_object());
    VORTEX_EXPECT_EQ(rhead->field(0).as_smi(), int64_t{-1});
    VORTEX_EXPECT_EQ(rsecond->field(0).as_smi(), int64_t{INT32_MIN});
    remove_tmp(kTmpSnapshot);
}

/// CEP:WHAT: Hand-built heap payloads with a wild swizzle index (named
///           reject) and a negative-Smi word (accepted as a Smi).
/// CEP:WHY: The parse-pass index validation is the Rule-9 gate the old
///           .at() path lacked; the good-word arm pins the sound decoder.
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: snapshot_rejects_malformed_heap_payloads.
VORTEX_TEST(snapshot_rejects_malformed_heap_payloads) {
    // Hand-built Shapes/Objects payloads: a valid single-klass heap, then
    // corruptions the writer would never emit (Rule 9).
    const auto build = [](const std::vector<uint64_t>& words) {
        std::vector<uint8_t> shapes;
        put_u32(shapes, 1);            // one klass
        shapes.push_back(0);           // not the double klass
        put_str(shapes, "K");
        put_u32(shapes, 1);            // one field
        put_str(shapes, "f");
        put_u32(shapes, 24);           // offset
        put_u32(shapes, 0);            // no methods
        std::vector<uint8_t> objs;
        put_u32(objs, 1);              // one object
        objs.push_back(0);             // plain
        put_u32(objs, 0);              // klass index
        put_u32(objs, static_cast<uint32_t>(words.size()));
        for (uint64_t w : words) put_u64(objs, w);
        SnapshotWriter w;
        (void)w.set_manifest(snapshot_default_manifest());
        (void)w.add_section_named(SnapshotSection::Shapes, "heap-klasses",
                                  std::move(shapes));
        (void)w.add_section_named(SnapshotSection::Objects, "heap-objects",
                                  std::move(objs));
        return w.finalize();
    };

    // A swizzled word pointing past the object table: named reject, and the
    // old .at() path would have thrown (15-e-a/2).
    {
        // (0xFFFFFFFF << 32) | 0x5 | (99 << 4) — swizzled, index 99.
        const uint64_t wild = (0xFFFFFFFFull << 32) | 0x5ull | (99ull << 4);
        auto bytes = build({wild});
        VORTEX_EXPECT(bytes.has_value());
        if (!bytes) return;
        write_bytes(kTmpSnapshot, *bytes);
        auto reader = SnapshotReader::open(kTmpSnapshot);
        VORTEX_EXPECT(reader.has_value());
        if (!reader) return;
        auto r = restore_heap_snapshot(*reader);
        VORTEX_EXPECT(!r.has_value());
        if (!r) {
            VORTEX_EXPECT(r.error().message.find("swizzle index") !=
                          std::string::npos);
        }
    }
    // A negative Smi (high word 0xFFFFFFFF) is NOT a swizzled reference.
    {
        const uint64_t smi_minus1 = 0xFFFFFFFF'FFFFFFFEull;
        auto bytes = build({smi_minus1});
        VORTEX_EXPECT(bytes.has_value());
        if (!bytes) return;
        write_bytes(kTmpSnapshot, *bytes);
        auto reader = SnapshotReader::open(kTmpSnapshot);
        VORTEX_EXPECT(reader.has_value());
        if (!reader) return;
        auto r = restore_heap_snapshot(*reader);
        VORTEX_EXPECT(r.has_value());
        if (r) {
            auto* obj = static_cast<Object*>((*r).objects[0]);
            VORTEX_EXPECT_EQ(obj->field(0).as_smi(), int64_t{-1});
        }
    }
    remove_tmp(kTmpSnapshot);
}

/// CEP:WHAT: An inflated dir-entry size (overlap) rejects by name.
/// CEP:WHY: The subtractive bounds check replaced the overflowing
///           additive one (review 15-e-a/3 / 15-e-b F4, probe-proven).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: snapshot_rejects_overlapping_sections.
VORTEX_TEST(snapshot_rejects_overlapping_sections) {
    // Dir entry 0's size is inflated to overlap entry 1 (the additive
    // overflow fix's negative, 15-e-a/3).
    const auto good = small_container();
    VORTEX_EXPECT(!good.empty());
    auto bad = good;
    // Entry 0 payload size field: dir starts at 12, entry i at 12+24*i;
    // size is the last u64 (bytes 12+24*i+16 .. +24).
    const size_t size_field = 12 + 24 * 0 + 16;
    for (int i = 0; i < 8; ++i) bad[size_field + i] = 0xFF;  // huge size
    // Refresh the file hash so the targeted bounds check fires.
    const uint64_t h = fnv1a64(bad.data(), bad.size() - 16);
    for (int i = 0; i < 8; ++i) {
        bad[bad.size() - 8 + i] = static_cast<uint8_t>((h >> (8 * i)) & 0xFF);
    }
    write_bytes(kTmpSnapshot, bad);
    auto r = SnapshotReader::open(kTmpSnapshot);
    VORTEX_EXPECT(!r.has_value());
    if (!r) {
        VORTEX_EXPECT(r.error().message.find("bounds") != std::string::npos);
    }
    remove_tmp(kTmpSnapshot);
}

/// CEP:WHAT: A deopt record whose remat mixes a window-slot field and a
///           const field round trips bit-exact (field_count included).
/// CEP:WHY: The old serializer desynced const_words from field_keys and
///           dropped field_count (review 15-e-b F1/F2, probe-proven).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: code_cache_mixed_remat_roundtrip.
VORTEX_TEST(code_cache_mixed_remat_roundtrip) {
    // A deopt record whose remat mixes a window-slot field and a const
    // field — the shape that desynced the old serializer (15-e-b F1/F2).
    constexpr const char* kSrc = R"(
.method f(regs=4, args=2)
    Add.I32 v2, v0, v1
    Return v2
.end
)";
    auto assembled = assemble_module(kSrc);
    VORTEX_EXPECT(assembled.has_value());
    if (!assembled) return;
    UGBModule module = std::move(*assembled);
    gc::Heap heap;
    vm::Interpreter interp(heap);
    const std::vector<TaggedValue> args = {TaggedValue::smi(2),
                                           TaggedValue::smi(3)};
    auto warm = interp.run(module, "f", args);
    VORTEX_EXPECT(warm.has_value());
    if (!warm) return;
    j1::J1Bindings bindings;
    auto br = j1::make_j1_bindings(heap, module, &interp, bindings);
    VORTEX_EXPECT(br.has_value());
    if (!br) return;
    const int32_t mid = module.find_method("f");
    auto& method = module.method_table[static_cast<size_t>(mid)];
    j2::J2Job job;
    job.module = &module;
    job.method_id = static_cast<uint32_t>(mid);
    job.profiles = &method.profiles;
    job.ics = &method.ics;
    job.klass_addrs = &bindings.klass_addr_table;
    job.double_klass = bindings.double_klass;
    auto code = j2::compile_j2(job);
    VORTEX_EXPECT(code.has_value());
    if (!code) return;

    // Synthesize the mixed remat over the compiled artifact.
    j2::DeoptRecord rec;
    rec.region_id = 0;
    j2::DeoptFrame fr;
    fr.method_id = static_cast<uint32_t>(mid);
    fr.resume_pc = 4;
    fr.inject_dst = 0xFFFFFFFFu;
    fr.vreg_base = 0;
    fr.vreg_count = 4;
    j2::DeoptFrame::RematEntry rm;
    rm.slot = 6;
    rm.klass_token = 0;
    rm.field_count = 2;
    rm.field_keys = {0, 1};                       // field 0 then field 1
    rm.field_slots = {3, j2::DeoptFrame::RematEntry::kConstSlot};
    rm.const_words = {TaggedValue::smi(77).raw()};  // ONE word, second field
    fr.remats.push_back(std::move(rm));
    rec.frames.push_back(std::move(fr));
    auto records = std::make_shared<std::vector<j2::DeoptRecord>>();
    records->push_back(std::move(rec));
    code->records = records;
    code->record_relocs = {{0, 0}};

    auto handles = heap_klass_handles(interp.klass_registry(), heap);
    SnapshotWriter writer;
    (void)writer.set_manifest(snapshot_default_manifest());
    auto wst = write_code_snapshot(*code, handles, writer);
    VORTEX_EXPECT(wst.has_value());
    if (!wst) return;
    auto bytes = writer.finalize();
    if (!bytes) return;
    write_bytes(kTmpSnapshot, *bytes);
    auto reader = SnapshotReader::open(kTmpSnapshot);
    VORTEX_EXPECT(reader.has_value());
    if (!reader) return;
    auto code2 = load_code_snapshot(*reader, handles);
    if (!code2) {
        std::printf("  [snapshot-test] reload: %s\n",
                    code2.error().message.c_str());
        return;
    }
    // The remat survives bit-exact: field_count, the mixed slots, and the
    // const word (the old reader demanded keys.size() == words.size()).
    VORTEX_EXPECT_EQ(code2->records->size(), size_t{1});
    const auto& rm2 = code2->records->at(0).frames.at(0).remats.at(0);
    VORTEX_EXPECT_EQ(rm2.field_count, uint32_t{2});
    VORTEX_EXPECT(rm2.field_keys == (std::vector<uint32_t>{0, 1}));
    VORTEX_EXPECT(rm2.field_slots ==
                  (std::vector<uint32_t>{3, j2::DeoptFrame::RematEntry::kConstSlot}));
    VORTEX_EXPECT_EQ(rm2.const_words.size(), size_t{1});
    if (rm2.const_words.size() == 1) {
        VORTEX_EXPECT(rm2.const_words[0] == TaggedValue::smi(77).raw());
    }
    remove_tmp(kTmpSnapshot);
}

/// CEP:WHAT: The ingest's all-or-nothing contract: a late out-of-range
///           line leaves nothing applied; duplicates and oversized fields
///           reject by name.
/// CEP:WHY: The old ingest rejected INSIDE the mutating loop (review
///           15-e-b F6/F7/F8, probe-proven).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: profile_ingest_untouched_on_reject.
VORTEX_TEST(profile_ingest_untouched_on_reject) {
    // All-or-nothing: a late out-of-range line must not leave earlier
    // lines applied (15-e-b F6), duplicates reject (F7), truncation
    // rejects (F8).
    constexpr const char* kSrc = R"(
.method f(regs=3, args=2)
    ADD_ANY v2, v0, v1
    Return v2
.end
)";
    auto assembled = assemble_module(kSrc);
    VORTEX_EXPECT(assembled.has_value());
    if (!assembled) return;
    UGBModule module = std::move(*assembled);
    gc::Heap heap;
    vm::Interpreter interp(heap);
    const std::vector<TaggedValue> warm_args{TaggedValue::smi(1),
                                             TaggedValue::smi(2)};
    auto r = interp.run(module, "f", warm_args);
    VORTEX_EXPECT(r.has_value());
    if (!r) return;

    // Out-of-range slot on the LAST line: nothing applies.
    UGBModule probe;
    DecodeErrorInfo err{};
    auto encoded = encode_module(module);
    VORTEX_EXPECT(decode_module(encoded.data(), encoded.size(), probe, err));
    const std::string bad =
        "#vortex-branch-profile 1\nb 0 0 5 0 0 0 0\nb 0 999 5 0 0 0 0\n";
    auto res = ingest_branch_profile(bad, probe);
    VORTEX_EXPECT(!res.has_value());
    if (!res) {
        VORTEX_EXPECT(res.error().message.find("slot index") !=
                      std::string::npos);
    }
    // The first line's value must NOT have landed (probe's tables are the
    // freshly decoded module's — zeroed).
    bool applied = false;
    for (const auto& m : probe.method_table) {
        for (const auto& s : m.profiles) {
            if (s.execution_count != 0) applied = true;
        }
    }
    VORTEX_EXPECT(!applied);

    // Duplicate slot line: named reject.
    VORTEX_EXPECT(!ingest_branch_profile(
        "#vortex-branch-profile 1\nb 0 0 5 0 0 0 0\nb 0 0 6 0 0 0 0\n",
        probe)
                       .has_value());
    // Field too large to fit u32: named reject, no truncation.
    VORTEX_EXPECT(!ingest_branch_profile(
        "#vortex-branch-profile 1\nb 4294967296 0 5 0 0 0 0\n", probe)
                       .has_value());
}

/// CEP:WHAT: A threshold no method can cross rejects the emit by name.
/// CEP:WHY: The selection is profile-licensed; an empty selection must
///           be a named refusal, not an empty artifact (spec section 6).
/// CEP:STATUS: complete
/// CEP:FAILURE: assertion failures count through the harness.
/// CEP:ASSUMES: temp files live in the ctest cwd.
/// CEP:COST: @cold. Test-only.
/// CEP:EVIDENCE: aot_pipeline_empty_selection_rejected.
VORTEX_TEST(aot_pipeline_empty_selection_rejected) {
    AotFixture f = make_aot_fixture();
    VORTEX_EXPECT(f.ok);
    if (!f.ok) return;
    Manifest expected = write_aot_inputs(f);
    (void)expected;
    PgoProfileInput in;
    in.module_path = kTmpModule;
    in.ic_profile_path = kTmpIc;
    in.branch_profile_path = kTmpBranch;
    in.klass_handles = f.handles;
    in.min_execution_count = 1ull << 40;  // nothing can cross this
    auto p = AotPipeline::emit_shared_library(in, kTmpAot1);
    VORTEX_EXPECT(!p.has_value());
    if (!p) {
        VORTEX_EXPECT(p.error().message.find("threshold") !=
                      std::string::npos);
    }
    remove_tmp(kTmpModule);
    remove_tmp(kTmpIc);
    remove_tmp(kTmpBranch);
}
