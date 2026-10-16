# Infra 3 — Startup Snapshots & AOT Pipeline

T0 and J1 are fast, but parsing and compiling core libraries on every startup is
wasteful. This system removes that cost.

Status: the M0 contract stub fixed the API surface (`include/vortex/infra/
snapshot.hpp`). M6-a lands the implementation: the versioned container, the
Rule-97/98 manifest protocol, the heap snapshot writer/reader, the shared code
cache round trip, profile persistence, and the PGO-AOT offline pipeline.
Laws: Rule 9 (artifacts are untrusted), Rule 18 (AOT preserves UGB observable
semantics), Rule 24 (compilation consumes frozen snapshots), Rule 97
(compatibility manifests), Rule 98 (verification before loading), Part I
"AOT / Snapshot Pipeline" (AOT is not a fifth tier).

## 1. The snapshot container (v1.1)

Every artifact this system produces is one container — a heap snapshot, a
shared-code-cache segment, and an AOT artifact differ only in which sections
they carry. Byte layout (little-endian throughout):

    SnapshotHeader          12 bytes
      u32 magic  = 0x5633534E   ('V3SN')
      u16 version_major = 1
      u16 version_minor = 1
      u32 section_count         (directory entries that follow)

    SnapshotDirEntry [section_count]   24 bytes each
      u8  section kind          (SnapshotSection: Objects, Shapes,
                                 InlineCaches, MachineCode, Metadata)
      u8  reserved (0)
      u16 name_len              (bytes; 0 = anonymous)
      u32 name_offset           (POOL-relative — see the name pool below)
      u64 payload_offset        (from file start)
      u64 payload_size

    name pool                 (the concatenated section names; sits between
                               the directory and the first payload)

    payload blobs (8-byte aligned; no inter-section padding gaps inside a blob)

    Trailer                 16 bytes
      u64 manifest_offset       (from file start; a Metadata-section payload;
                                 advisory — the loader reads the FIRST
                                 Metadata section by kind)
      u64 file_hash             FNV-1a 64 over every byte before the trailer

Integrity protocol (Rule 9): the reader verifies, in order — size, file
hash, magic, version match, directory bounds (offset+size inside the file,
sections non-overlapping, subtractive form), each section's payload hash
(FNV-1a 64, stored as the first 8 bytes of every payload), and the name
pool (names live inside [pool base, smallest payload offset)). Every
failure is a named `Diagnostic`
(`snapshot: <what>`), never a crash, never a partially-constructed runtime
object. Sections are opaque to the container: the kind enum and the hash
discipline are the only things the container layer knows.

The `SnapshotWriter::add_section` surface from the stub is kept; `finalize()`
now emits the layout above. A writer refuses a second `finalize()` and refuses
section kinds repeated with the same name (named failures). `SnapshotReader::
open` maps the file, runs the verification protocol, and exposes
`section_count()`, `section_payload(kind, name)`, and the manifest (section 2).
The reader owns its own copy of the bytes (the tests run under ASan; mmap
sharing is an optimization with no semantic content at this scale and the
shared-code-cache mapping story is section 5).

## 2. The compatibility manifest (Rules 97/98)

The manifest is the FIRST Metadata section of every container, serialized by
`ManifestWriter` and verified by `ManifestVerifier`. Nine fields, in order,
each length-prefixed (`u32 len, bytes`; u64 fields as 8 raw bytes):

    u32 ugb_version               (UGB_LEVEL_CURRENT)
    u32 compiler_version          (VORTEX_COMPILER_VERSION, bumped with the
                                   pass pipeline's observable contract)
    u64 pass_pipeline_hash        (FNV-1a over the enabled stage list)
    str target_arch               ("x86-64")
    str target_feature_set        (the CPU dispatch feature string, e.g. "base"
                                   — artifacts are per-feature-set)
    u64 abi_hash                  (FNV-1a over the J1 entry/OSR ABI layout
                                   constants — spill base, frame words)
    u64 runtime_config_hash       (FNV-1a over the runtime config knobs the
                                   artifact's code depends on)
    u32 dependency_fingerprint_count
    dep [n]                       (str name, u64 fingerprint — the module
                                   hashes the artifact was compiled against)
    u32 security_policy_version   (the W^X/security manager policy version)

`ManifestVerifier::verify(actual, expected)` compares field by field and
returns the FIRST mismatch as a named diagnostic naming both values
(`snapshot: manifest <field> mismatch: have X want Y`). Rule 98's checklist
maps exactly: manifest compatibility (all nine fields), integrity (the
container hash protocol), dependency validity (fingerprints), target feature
support (feature string must be a subset the loader's CPU supports — the
dispatch layer's `supports(feature)` verdict), ABI compatibility (abi_hash),
security policy (policy version). On any mismatch the artifact is REJECTED —
the loader returns the diagnostic and the caller falls back to the JIT path
(Rule 11: refusal degrades, never blocks).

## 3. Heap snapshots

`HeapSnapshotWriter::write(const gc::Heap&, const runtime::KlassRegistry&)`
serializes the logical heap; `HeapSnapshotReader::restore(config)` rebuilds it
in a fresh process. Two sections:

**Shapes** — the klass table: `u32 klass_count`, then per klass:
`u8 is_double` (the boxed-double klass links to the fresh heap's own double
klass on restore — the heap owns that klass object; every other klass is
recreated), name (length-prefixed), `u32 field_count`, per field (name,
`u32 offset`), `u32 method_count`, per method (name, `u8 implementation_kind`,
`u32 ugb_method_id`).

**Objects** — `u32 object_count`, then per object, in address order:
`u8 kind` (0 = plain Object, 1 = ArrayObject, 2 = boxed double),
`u32 klass_index`, then the payload: plain — `u32 field_count` (the KLASS's
field count — the authoritative layout; alignment pad beyond the last field
is zeroed at allocation and is not serialized) + field words; array —
`u32 length` + element words; boxed double — 8 bytes IEEE binary64.

Swizzle protocol: every field/element word is stored raw EXCEPT heap-pointer
words (tag nibble `0b0001`), which are rewritten to
`0xFFFF'FFFF'0000'0000 | (index << 4) | 0x5` — the fixed high word plus a
low-nibble tag of `0b0101`, a value NO legal TaggedValue can take (Smi
words have bit0 == 0; null/undefined/false/true use 0x3/0x7/0xB/0xF; live
heap pointers use 0x1). Negative Smis share the 0xFFFFFFFF high word, so
the NIBBLE is what makes the decode sound — the low 28 bits (above the tag
nibble) carry the index into the Objects section's object list (28 bits =
268M objects, above the 1M restore cap). Restore validates every swizzled
index against the object count BEFORE anything is allocated (Rule 9), then
walks the list in order, allocates through the normal `Heap::allocate_*`
paths (the allocation ORDER is the snapshot order, so the fresh layout is
offset-deterministic), and rewrites each pointer word: `index -> restored
address, tag reapplied`.

Not serialized — GC runtime state, by contract: mark/forwarded/pinned header
bits, Brooks pointers (restore allocates with `brooks == self`), TLAB tops,
card tables, SATB buffers, remembered sets, region state. A snapshot is a
LOGICAL heap image, not a core dump: restoring into a heap that has run
collections still yields a correct, compact heap.

Determinism contract (the shared-code-cache enabler): restoring the same
snapshot bytes into a fresh `Heap` + `KlassRegistry` twice produces the same
LAYOUT — object i sits at `heap_base + O_i` with identical O_i, and every
klass restores with the same name, order, and field-slot offsets. Heap-
relative geometry is a pure function of the snapshot bytes. Raw base
pointers are NOT snapshot data: the heap buffer is malloc'd and the
KlassRegistry objects are process-allocated, so cross-process absolute
addresses require the re-binding step below (section 4) and, for the
buffer itself, a fixed mapping — both are deployment-batch work, not
capture/restore semantics. The test suite pins the offset identity
(`restore == restore`, offset-for-offset) and the klass layout identity.

Identity contract: klass identity is by index (restore preserves the
index->klass mapping); object identity is preserved within one restore (two
references to one object restore as one object). The snapshot captures a
CONSISTENT state — the writer runs at a quiescence point (single-mutator
model, Rule 24: no guest execution during capture).

## 4. The shared code cache

`CodeSnapshotWriter::write(const j2::J2Code&)` emits a MachineCode section
(plus a Metadata payload for the manifest). Payload layout:

    u32 method_id
    u32 entry_offset
    u32 osr_entry_offset (0xFFFFFFFF when absent)
    u32 osr_pc_count + u32[osr_pc_count]
    u32 code_size + code bytes            (address-free: Rule 56 — the
                                           deopt-record immediates are ZERO
                                           in the compiled bytes)
    gc_maps blob (raw)
    deopt_records blob (raw)
    u32 region_count + per-region descriptor (the RegionTable's serialized
                                           form: the RBPD wire format,
                                           docs/deopt-rbpd.md)
    u32 record_reloc_count + {u32 code_offset, u32 record_index} per reloc
    u32 record_count + per record {region_id, suspension, frames (with
        remats: slot, klass_token, field_count, field_keys, field_slots,
        const_words — keys and slots are parallel, const_words parallel to
        the kConstSlot entries)} — the LIVE records the stubs reference
    u8  has_summary + the EscapeSummary fields when set
    u8  budget_exceeded
    u32 klass_handle_count + {str klass_name, u64 baked_address} per handle

The klass-handle table is the reload-verification story: `compile_j2` bakes
klass pointers as imm64s (compile-time inputs, shared by every compile of
one runtime instance). The snapshot writer does not re-scan the code — the
RUNTIME side registers the handle set it passed via `J2Job::klass_addrs`
and `double_klass`; the writer records name+address pairs; the loader
checks each baked address against the LOADING runtime's klass addresses.
All match → publish. Any drift → named reject (Rule 98: dependency
validity). This makes the check honest without touching the J2 emitter.
Scope note: the verification validates an IN-PROCESS reload (and any
future loader that can re-bind imm64s); a full cross-process reload needs
the recorded handle set turned into a RELOCATION table so the loader can
re-patch the baked immediates against the fresh process's klass objects —
tracked with the deployment batch (section 6 deviation).

`CodeSnapshotReader::load` reconstructs a `J2Code` and publishes through the
normal `publish_j2` path (W^X, record relocations, region table, GC record) —
the reload shares one publication contract with the JIT (no second emission
path). DoD: the reloaded executable runs a method at parity with the fresh
compile, and the reloaded `J2Code` bytes are bit-identical to the original
(the zero-immediates property makes the round trip byte-exact).

## 5. Profile persistence (.ic / .branch)

PGO-AOT ingests production profiles. The runtime dumps the live slot tables:

`.branch` — the ProfileSlot table (one text line per non-empty slot):
    b <method_id> <slot_index> <execution_count> <failure_count>
      <ring_size> <tag[ring]> <branch_not_taken> <branch_taken>
`.ic` — the IcSlot table:
    i <method_id> <slot_index> <state> <poly_count> <failure_count>
      <klass_id:target>[0..1] <klass_id:target>[poly]

Versioned headers (`#vortex-branch-profile 1` / `#vortex-ic-profile 1`);
ingest validates every line (field counts, ranges, duplicate slot = named
reject) and restores the tables bit-exactly (Rule 9: profiles are untrusted
input). Formats are text so they diff and survive tooling; the volumes are
small (slot tables, not traces).

## 6. The PGO-AOT pipeline

`AotPipeline::emit_shared_library(input, output_path)`:

1. Load the UGB module, the `.ic` and `.branch` profiles (validated ingest).
2. Select methods: `execution_count >= aot_min_execution_count` (named knob,
   default 1000) — the profile IS the selection license (Rule 24's frozen
   profile snapshot: the dump is the frozen form).
3. Compile each selected method through the J4 deterministic pipeline (the
   fixed J3-driver stage order, no search — the M4 determinism contract
   carries over: same inputs, same bytes).
4. Emit ONE container: MachineCode sections (one per method, section-4
   payload) + the Rule-97 manifest. The manifest's dependency fingerprints
   cover the module hash and every profile hash; the pass-pipeline hash is
   the J4 stage list.

Load path (the deployment boot): open container → verify protocol (section 1)
→ manifest verify against the loading runtime (section 2, including the CPU
feature set) → load + publish the code sections (section 4; in-process the
klass handles verify name+address, cross-process they become the
relocation input) → the DRIVER restores the companion heap snapshot
(offset-deterministic layout) and dispatches. `load_artifact` covers the
container/manifest/code layers; the heap restore is the driver's step.
Tiering treats an AOT-loaded method as pre-published: no recompile,
deopt/lower-tier fallback intact (Rule 11).

DEVIATION, documented: the container is the deployable artifact. Wrapping it
as a real `.so`/`.dll` (system linker, dlopen packaging) is deferred to the
M6 packaging batch — the loading contract and all verification semantics are
identical, and no test or consumer in this repo needs an ELF wrapper yet.

## 7. Definition of done (verifiable criteria)

- `snapshot_container_rejects_malformed`: bad magic, wrong version, bit-flip
  inside a payload, truncated file, overlapping sections — each a NAMED
  reject, no crash (Rule 9).
- `snapshot_manifest_mismatch_rejected`: each of the nine Rule-97 fields
  mismatches → named reject naming the field; all-match → accepted (Rule 98).
- `heap_snapshot_roundtrip_parity`: guest state built through T0 → snapshot →
  restore into a fresh runtime → continue execution → identical observable
  results; klass and object identity preserved.
- `heap_snapshot_restore_deterministic`: two restores of one snapshot →
  identical layout offsets, byte-identical klass layouts.
- `code_cache_reload_parity`: compile → snapshot → reload (publish path) →
  execute → same results as the fresh compile; reloaded code bytes
  bit-identical; klass-handle mismatch → named reject.
- `profile_dump_restore_roundtrip`: slot tables bit-exact after a dump/
  ingest cycle; malformed lines rejected by name.
- `aot_pipeline_deterministic`: two runs over the same module + profiles →
  byte-identical artifacts.
- `aot_artifact_load_execute_parity`: artifact container verifies, loads,
  executes at parity with the JIT-fresh compile of the same method.
- CEP&CC batch 3: the snapshot tree (new files) ships CEP&CC-complete from
  birth; touched legacy files keep a zero lint delta; the suite stays green
  on Release and ASan/UBSan.
