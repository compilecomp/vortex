# Infra 6 — Observability, Debugging & Tooling System

You cannot ship a black box. The JIT must dynamically expose its internals to OS
tools. The M6-d batch lands the **observability floor** — the external
performance review's R40 ("ship the observability floor early... enterprise
adoption is gated more on this section than on any percentage"): the
`perf-jitdump` writer with its `perf-<pid>.map` sidecar, the safepoint TTSP
log, GC pause-ring cause labels, and the TLAB-refill allocation sampler.
Batch e completes the item: the GDB JIT interface, the managed sanitizers,
deterministic replay, and the jitdump metadata interchange (real symbol
names + source-line debug entries).

## Mechanisms

- **JIT profiler interface (perf/VTune).** The runtime writes a
  `perf-jitdump` v1 file mapping dynamically generated machine-code ranges to
  named symbols, plus the text `perf-<pid>.map` sidecar, so `perf record`
  attributes guest-language hotspots to JIT frames.
- **Safepoint/TTSP telemetry.** Every suspension the handshake manager drives
  — stop-the-world freezes and single-thread handshakes — records its cause,
  time-to-safepoint (request/arm → last acknowledge), and pause body (all
  acknowledged → thaw). This is the p99 attribution source the performance
  review's measurement plan requires.
- **GC cause labels.** The M5 pause ring (docs/gc-icggc.md 8.5) gains a
  cause per pause event: allocation-slow-path young collections, occupancy
  triggered old cycles (driver convenience included — it routes through the
  same trigger; `ExplicitRequest` is the adaptive-initiation batch's
  reserved label) — so p99 debugging starts from
  "what started it", not just "how long".
- **Allocation profiling.** JEP-331-shape TLAB-refill sampling: the heap's
  refill path (the warm allocation slow path, never the hot bump) feeds an
  interval-driven sampler whose samples land in a bounded ring.

## perf-jitdump v1 file format (normative)

The file is the perf JIT dump interface (kernel tools/perf Documentation,
`jitdump.spec.txt`): all values little-endian, packed, UNALIGNED — writers
append byte-wise, readers walk by field width, never by alignment.

- **Header (40 bytes, written once at open):** `u32 magic = 0x4A695444`
  ("JiTD"), `u32 version = 1`, `u32 total_size = 40`, `u32 elf_mach = 62`
  (EM_X86_64), `u32 pad1 = 0`, `u32 pid` (getpid), `u64 timestamp`
  (CLOCK_MONOTONIC ns at open), `u64 flags = 0`.
- **Record framing:** `u32 id`, `u32 record_size` (the TOTAL byte count
  including this 16-byte header — a reader advances by it), `u64 timestamp`
  (CLOCK_MONOTONIC ns; non-decreasing across the file).
- **JIT_CODE_LOAD (id 0):** `u32 pid`, `u32 tid` (gettid), `u64 vma` (the
  code mapping's base — the executable's memory base), `u64 code_addr` (the
  function's address), `u64 code_size`, `u64 code_index` (monotonic per
  session), then the NUL-terminated function name, then `u32
  nr_debug_entries = 0` (line-level debug entries are batch e's DWARF work —
  the field is written, zero-filled, and its format is already reserved).
- **JIT_CODE_CLOSE (id 3):** framing only, written at session stop.
- **Self-mmap:** the format requires the producer to `mmap` the jitdump file
  `PROT_READ|PROT_EXEC` (page-aligned) so perf discovers it — open() maps the
  header-bearing initial extent and keeps the mapping for the session
  lifetime; stop() unmaps.
- **Default location:** `/tmp/perf-<pid>.jitdump` (the live-run convention).
  An explicit path override exists for tests and redirected tooling.

Errors are named Result failures: open on an uncreatable path, open while a
session exists (ONE jitdump producer per process — perf's requirement),
record/stop on a closed session. Timestamps come from one
CLOCK_MONOTONIC read per record; the writer never fabricates or reorders
them.

## perf-<pid>.map sidecar (normative)

One line per code load, `<hex_start> <hex_size> <name>` — start and size in
lowercase hex WITHOUT the `0x` prefix, name with spaces and newlines replaced
by `_` (the format is whitespace-delimited; HotSpot's sanitizer is the
precedent). Appended per record, same record flow as the jitdump CODE_LOAD,
default `/tmp/perf-<pid>.map`. Files persist after stop() (perf reads them
after process exit); tests use explicit paths and clean up after themselves.

## JitSession — the one producer (normative)

A process singleton owns the jitdump writer and the perf-map. The code
publish path (the ONE W^X publish consumer: J1's `publish_baseline` and the
J2/J3/J4 family's publish) calls the session's `record_code(base, size, name)`
AFTER the bytes are final. Passive-hook failure semantics: a telemetry
recording failure NEVER fails the publish — the session stores the last
error for the caller to observe (`last_error()`), because compiled code that
runs correctly must not be rejected by a full disk. With no session open the
publish path pays one null-pointer branch (@warm path, publish is cold).

Symbol names in this batch are synthetic and stable:
`vx_<tier>_m<method_id>` (tier in {j1, j2}; the J2/J3/J4 family publishes
through the J2Shape path today). Real UGB method names ride the metadata
interchange upgrade with the batch-e DWARF entries — the name is the only
field affected; record framing is unchanged.

## Safepoint TTSP log (normative)

`SafepointEvent { kind: Freeze | Handshake, cause (SuspensionKind value),
ttsp_ns, body_ns, threads, timed_out, generation }` in a bounded ring (512
events; on overflow the ring clears — the same Rule-28 discipline as the GC
pause ring). Semantics:

- **Freeze:** `ttsp_ns` = freeze entry (taken just before the shared
  deadline computation, which precedes arming) → last acknowledge (or
  timeout deadline); `body_ns` = last acknowledge → thaw_all (rolled-back
  freezes record the full frozen window at rollback); `threads` = the alive
  set size; `timed_out` = the rollback case. `generation` = the freeze
  generation. Boundary slop: the stamps are read at the manager's lock
  (re)acquisition — sub-microsecond over-attribution under contention is
  the documented measurement shape for this batch.
- **Handshake:** `ttsp_ns` = request → acknowledged wait; `body_ns` = 0 (a
  single-thread handshake has no body in this batch); `threads` = 1.

The handshake manager owns a `SafepointLog*` (settable, raw pointer, no
ownership, null default — zero cost when unset). Emission happens inside the
manager's existing critical sections; the log never blocks and never fails
the handshake (telemetry failure = the event is dropped, never surfaced as
a runtime error — the handshake's own Result semantics are untouched).
Percentile helpers (nearest-rank p99/max) mirror the GC stats shape.
Preemptive (guard-page) suspensions join this log with the emitted-poll
batch (R31) where they become a common path; the M6-b budget tests stay the
preemption telemetry until then.

## GC pause-ring cause labels (normative)

`PauseEvent` gains `cause: PauseCause { Unspecified = 0, AllocationSlowPath,
OccupancyTrigger, ExplicitRequest }` (defaulted — existing aggregate
initialization and the two-argument `record_pause` keep compiling; the M5
DoD tests are untouched). The engine carries one cycle-scoped cause set at
cycle start: the occupancy trigger is the ONLY start mechanism today —
`maybe_start_cycle` stamps `OccupancyTrigger`, and `run_cycle` routes
through the same trigger check (the driver convenience does not force-start
below the trigger; the M5 contract is occupancy-only initiation).
`ExplicitRequest` is the RESERVED label for the adaptive-initiation batch
(P1 R16), which adds the initiation modes that can produce it; the young
GC's pause timer = `AllocationSlowPath`; phase and slice timers inherit the
cycle cause. `record_pause(kind, dur, cause)` is the new overload;
`PauseTimer` gains the matching constructor.

## Allocation sampler (normative)

`AllocSampler` — process singleton, fed by `Heap::refill_tlab` AFTER a
successful rewindow (@warm; zero hot-path cost — the fast-path bump never
calls it):

- `interval_bytes` (default 512 KiB, JEP-331's default class): refill bytes
  accumulate; crossing the interval records one sample. Changing the
  interval starts a FRESH accumulation window (bytes counted under the old
  interval never fire samples under the new one).
- Sample: `{ timestamp_ns (CLOCK_MONOTONIC), refill_bytes, cumulative_bytes,
  site_id = 0 }`. Ring bounded at 4096 with the Rule-28 clear-on-overflow
  discipline. Counters: `refills_observed`, `samples`,
  `total_refill_bytes` — monotonically increasing, never cleared.
- **Attribution deviation (documented):** JEP 331 attributes each sampled
  refill to the allocating stack. Vortex's precise unbiased stack walk
  arrives with the emitted-poll batch (R31); in this batch `site_id` is
  written 0 (unknown) and the field is reserved. The sampling skeleton,
  rate control, ring, and telemetry are the deliverable; attribution is the
  named upgrade, not a silent gap.

## GDB JIT interface (normative)

The canonical GDB JIT debug interface (gdb/jit.h ABI), implemented as the
well-known client symbols the debugger discovers at attach time:

- `struct __jit_debug_descriptor { uint32_t version; uint32_t action_flag;
  struct jit_code_entry* relevant_entry; struct jit_code_entry*
  first_entry; }` — version 1, zero-initialized at static-init time (gdb
  reads it even when nothing is registered; the descriptor exists
  unconditionally, zero hot-path cost).
- `extern "C" void __jit_debug_register_code(void)` — a real, never-inlined,
  never-eliminated empty function (`used` + `noinline`); the debugger sets
  its breakpoint here.
- `jit_code_entry { next_entry, prev_entry, symfile_addr, symfile_size }`
  (the canonical doubly-linked gdb/jit.h shape) — one entry per registered
  code blob, chained through `first_entry`; head insertion on register,
  symmetric unlink on unregister. `relevant_entry` names the entry the
  action consumed and dangles after an unregister BY CONTRACT (the
  debugger consumes it at the breakpoint hit, never later).
- Actions: `JIT_NOACTION = 0`, `JIT_REGISTER_FN = 1`,
  `JIT_UNREGISTER_FN = 2`.

**Symfile.** Each registered blob gets an in-memory ELF64 little-endian
`ET_EXEC` image: ELF header (`e_machine = 62` EM_X86_64, `e_entry = 0`),
one `PT_LOAD` (`PF_R|PF_X`) covering the image, and the sections
`[NULL, .text, .symtab, .strtab, .shstrtab]`. `.text` holds a byte copy of
the registered code (registration SNAPSHOTS the bytes — the live W^X
mapping is never referenced afterward); `.symtab` carries one
`STT_FUNC`/`STB_GLOBAL` symbol with `st_value = code_addr` (absolute) and
`st_size = code_bytes`. Emission is deterministic (fixed section order,
zero padding, no timestamps) — golden-pinnable.

**Semantics.** `register_code(code, size, symbol)`: named failures for
null/zero-size and a duplicate live base; otherwise build the symfile,
link the entry at the head, set `action_flag = JIT_REGISTER_FN`, call the
breakpoint function. `unregister_code(code)`: named failure for an unknown
base; otherwise unlink, set `JIT_UNREGISTER_FN`, call the breakpoint
function. One mutex serializes descriptor and chain mutation
(@cold only — registration never runs in signal context). The publish
path registers unconditionally after the bytes are final (one extra copy
of published code, @cold — no feature flag); an RAII token on the
executable unregisters at teardown, so the descriptor never outlives the
code.

## Managed sanitizers (normative)

ASan-for-UGB: a guest-heap shadow memory that detects the GC-corruption
classes — access into **evacuated-from** copies and **released**-region
spans — at the boundaries guest code and foreign code actually touch.

- `SanitizerPolicy` is the validated process config: `guest_asan`
  supported (default false); `guest_tsan` is RESERVED — validation refuses
  enabling it by name until the atomics capability pack ships (a named
  refusal, never a silent ignore).
- `GuestShadow` — a side structure over the guest heap span, 1 byte per 8
  heap bytes (`kShadowShift = 3`), constructed exactly like the card table
  (span-indexed, no per-object metadata). Values: `0` addressable,
  `kShadowEvacuated` (evacuated-from old copy), `kShadowReleased`
  (released/recycled-pending region span).
- Producers (all @cold): the ICGGC engine poisons the evacuated-from span
  during evacuation and the released region's span at region release; a
  region recycle (cold path) unpoisons. The bump-allocated young space is
  NEVER poisoned (per-alloc unpoison would be hot-path cost for no
  detection class).
- Consumers: T0's array load-store ops and the IC-hit field path consult
  the shadow when the policy is enabled (the sites with clean named-error
  paths in the interpreter loop); the FFI boundary validates Ptr-arg
  spans (`GuestToNativeStub::validate_ptr_args` — the C++ boundary check
  the machine-code type gates cannot do); the public `verify_span(addr,
  bytes)` API is the check everything routes through. The cached-field
  helpers' internal plumbing (`get/set_field_cached`) rides the R31
  metadata batch (the same layout work) — the IC-hit path already covers
  the hot field reads. Cost contract: policy DISABLED — one relaxed
  global-bool load per checked op (default); policy ENABLED — one shadow
  byte load + compare per checked op.
- Violation shape: a NAMED trap — the interpreter raises the sanitizer
  error carrying kind + violating address (adjacent to the Debug.Trap
  semantics); `verify_span` returns the same named failure. Detection
  never aborts the process and never corrupts: fail-fast into the trap
  semantics, by name.

## Deterministic replay (normative)

The honest batch-e scope: the engine's **decision recorder** and the
differential **first-divergence checker** — the property pinned is "same
program + same input + same policy → same decision stream" (the J4
determinism DoD, extended engine-wide). Exact re-execution scheduling is
NOT this batch.

- `ReplayEvent { uint64_t seq; uint32_t kind; uint64_t a; uint64_t b; }` —
  documented payload packing per kind: TierHandoff (a = method_id,
  b = from | to << 32), OsrEntry (a = method_id, b = bytecode pc),
  IcTransition (a = from | to << 32, b = the entry's klass id — the
  state machine carries no site identity; the stream shape is the
  contract), GcCycleStart (a = cause), Deopt (a = method_id, b = cause),
  Publish (a = method_id, b = tier ordinal), SanitizerTrap (a = violating
  address low bits, b = kind).
- Ring bounded at 65536 events with the Rule-28 clear-on-overflow
  discipline; `seq` is monotonic and survives the clear (it counts
  events, not slots).
- Producers, all behind ONE runtime flag (`replay_enabled`, default
  false — zero events, zero cost when off): the tiering driver's handoffs,
  T0's IC state transitions, the GC engine's cycle starts, the RBPD
  verdict consumption sites, the publish path, sanitizer traps.
- `compare_streams(a, b)` walks both rings O(n) and reports
  `{ identical, first_divergence, kind, expected (a,b), actual (a,b) }` —
  the first divergence IS the diagnosis.

## Real symbol names and jitdump debug entries (normative)

The metadata interchange upgrade the M6-d floor reserved:

- The J1/J2 compile results gain `symbol_name` (the input `UGBMethod`'s
  name; may be empty); J1 additionally gains `line_map` — sorted
  `(native offset, line)` pairs built by the compiler from the method's
  `DEBUG_SRCPOS` metas (the stencil layout is 1:1 with the bytecode, so
  the mapping is exact). The publish path passes both through.
- **Family deviation (documented):** the J2/J3/J4 family publishes through
  the J2 path with real symbol names but EMPTY line maps — the graph-
  scheduled layout has no 1:1 pc→offset map, and fabricating one is worse
  than shipping zero entries (Rule 120). Exact line entries for the family
  ride the R31 emitted-poll batch's metadata work, which touches the same
  layouts.
- `record_code` gains the named+line overload: CODE_LOAD carries the real
  (sanitized) name and `nr_debug_entries = line_map.size()`, each
  `JIT_CODE_DEBUG_ENTRY` record `{ u64 code_addr = base + offset, u32
  line, u32 column = 0 }` following the name. Methods without
  `DEBUG_SRCPOS` ship zero entries — nothing is fabricated.
- Synthetic fallback: an empty name publishes as `vx_<tier>_m<method_id>`
  (the M6-d shape — existing constructions and tests stay valid). The
  jitdump CODE_LOAD carries the RAW real name (the format is
  NUL-terminated and tolerates spaces); the perf-map sidecar — whose
  format is whitespace-delimited — carries the SANITIZED name (spaces and
  newlines to `_`). Duplicate real names may produce duplicate map lines
  (perf resolves by address range; the M6-d sanitizer precedent is
  HotSpot's).

## Vortex API

`include/vortex/infra/observability.hpp`:

- `JitDumpWriter` — instance-based writer implementing the format above
  (open/close/record_code_load with optional debug entries, is_open,
  file_size).
- `PerfMap` — instance-based sidecar writer (open/close/record).
- `JitSession` — the singleton facade: `start(path_jitdump, path_map)` /
  `start_default()` / `stop()`, `record_code`, `active()`, `last_error()`;
  the publish path's only touchpoint.
- `SafepointEvent` / `SafepointLog` — the TTSP ring + percentile helpers.
- `AllocSample` / `AllocSampler` — the refill sampler + `alloc_sampler()`
  singleton.
- `PauseCause` (gc side, collector.hpp) — the pause-ring labels.
- `TierEventSink` — unchanged (M0-real, consumed by `vx stats`).
- `GdbJitInterface` — the canonical client interface: `register_code` /
  `unregister_code` (named failures, mutex-serialized), the unconditional
  `__jit_debug_descriptor` + `__jit_debug_register_code` symbols, and the
  deterministic ELF64 symfile emitter.
- `SanitizerPolicy` — validated process config (guest_asan supported,
  guest_tsan a named refusal); `GuestShadow` — the span-indexed side
  structure (1 byte per 8 heap bytes) with `verify_span(addr, bytes)` as
  the one check everything routes through.
- `ReplayRecorder` — the decision ring (`record_event` with the documented
  kind packing, Rule-28 bounded, monotonic seq) + `compare_streams` (the
  first-divergence report) + the `replay_enabled` runtime flag.
- `record_code` — the new named+line overload (real symbols, JIT_CODE_DEBUG
  _ENTRY records); the M6-d signature keeps compiling as the synthetic
  fallback.

`tools/vx`: `run` and `stats` gain `--jitdump[=PATH]` (start_default or the
explicit path for the process lifetime; stopped at exit); `stats` prints the
heap stats, the sampler summary (interval, refills observed, samples, total
refill bytes), and the safepoint log summary (events, ttsp p99/max) after
the run — the telemetry a cold `vx stats` can reach without a GC engine.

## DoD tests

1. `jitdump_header_matches_format` — magic/version/total_size/elf_mach/pid/
   timestamp/flags, 40 bytes, written once.
2. `jitdump_code_load_record_roundtrip` — a recorded load parses back: id 0,
   record_size == bytes written, timestamp >= header timestamp, pid/tid,
   vma/code_addr/code_size/code_index, exact name bytes, nr_debug_entries 0.
3. `jitdump_timestamps_monotonic` — record sequence timestamps are
   non-decreasing.
4. `jitdump_close_record_and_stop` — stop writes id-3 framing, unmaps, closes;
   the file is a complete parse.
5. `jitdump_double_open_rejects_named` — a second session start is a named
   failure while one is active; after stop, restart succeeds.
6. `jitdump_record_after_stop_rejects_named`.
7. `jitdump_self_mmap_is_exec_and_page_aligned` — the session's mapping
   exists, page-aligned, PROT_EXEC (probed via /proc/self/maps).
8. `perfmap_line_format_and_sanitization` — hex start/size, no `0x`, names
   with spaces sanitized to `_`.
9. `publish_path_records_jit_code_loads` — with a session open, publishing a
   J1 baseline executable and a J2 executable appends CODE_LOAD records whose
   code_addr/code_size match the executables' memory ranges (and the perf-map
   gains the same lines); with no session, publish is unchanged.
10. `publish_survives_telemetry_failure` — an uncreatable telemetry target
    (open failed) never fails a publish; the session reports the stored
    error by name.
11. `handshake_freeze_records_ttsp_and_body` — freeze_all over polling
    fibers records one Freeze event: ttsp > 0, body > 0, threads == the
    alive set, timed_out false, generation matches.
12. `handshake_single_thread_records_handshake_event` — request/acknowledge
    round trip records ttsp > 0, threads 1.
13. `handshake_timeout_records_timed_out_freeze` — a rolled-back freeze
    records timed_out true.
14. `safepoint_log_ring_is_bounded` — overflow clears, counters keep the
    event count (Rule 28).
15. `gc_pause_ring_carries_causes` — occupancy-triggered cycle events carry
    OccupancyTrigger (run_cycle routes through the same trigger), young GC
    carries AllocationSlowPath; legacy two-arg record_pause still compiles
    and lands Unspecified; ExplicitRequest is reserved (no producer in
    M6-d — the adaptive-initiation batch introduces it).
16. `alloc_sampler_interval_and_ring` — refill sampling fires at the
    interval boundary, counters are exact, ring overflow clears, the heap's
    fast path allocation count is unaffected by sampling state.
17. `vx_stats_prints_telemetry_summary` — GATE-VERIFIED, not unit-tested:
    the unit binary cannot spawn the vx driver portably, so the gate
    procedure runs `vx stats <sample> --entry main --jitdump=<tmp>` and
    asserts the heap / alloc_sampler / safepoints summary lines and the
    jitdump stop line appear.
18. `gdb_descriptor_and_breakpoint_symbols_present` — the descriptor is
    zero-initialized (version 1, no entries) and both canonical symbols
    resolve at link time (the runtime links the core statically — a static
    archive does not populate .dynsym, so the pin addresses the symbols
    directly instead of dlsym).
19. `gdb_register_builds_parseable_elf_symfile` — after register_code the
    entry is linked at first_entry, action_flag = JIT_REGISTER_FN, the
    symfile parses as ELF64/EM_X86_64/ET_EXEC, .symtab carries the
    STT_FUNC symbol with st_value == code_addr and st_size == code_bytes,
    and .text byte-matches the registered code.
20. `gdb_unregister_removes_entry_and_sets_action` — after unregister the
    chain is empty, action_flag = JIT_UNREGISTER_FN.
21. `gdb_named_failures` — duplicate live registration, unregister of an
    unknown base, null/zero-size: all named rejects.
22. `gdb_multiple_entries_chain_in_order` — three registrations chain
    head-first in registration-reverse order; all symfiles parse.
23. `sanitizer_policy_validation_refuses_guest_tsan_named` — enabling
    guest_tsan is a named validation refusal; guest_asan enables cleanly.
24. `sanitizer_evacuated_copy_detected` — after a forced evacuation under
    the policy, verify_span on the evacuated-from span is a named trap
    carrying the kind; the same span reads clean before evacuation.
25. `sanitizer_released_region_detected` — region release poisons the
    span; verify_span reports it; recycle unpoisons.
26. `sanitizer_ffi_ptr_boundary_validated` — a Ptr-arg span is routed
    through verify_span at the boundary; a poisoned span is a named
    boundary failure.
27. `sanitizer_disabled_no_detections` — policy off: poisoned spans are
    NOT reported (the check short-circuits on the global gate; zero
    events, zero traps).
28. `replay_identical_runs_identical_streams` — two identical single-
    mutator runs with replay_enabled produce identical streams (the
    engine-wide determinism property).
29. `replay_first_divergence_located` — an injected difference between two
    streams is reported at the right index with the right kind and both
    payloads.
30. `replay_ring_bounded_rule28` — overflow clears the ring; seq keeps
    counting (Rule 28).
31. `replay_disabled_zero_events` — with the flag off no producer records
    anything (the cost contract).
32. `jitdump_real_name_and_debug_entries` — a compiled method with a real
    name and DEBUG_SRCPOS metas publishes CODE_LOAD with the raw real
    name and parseable debug entries (addr = base + offset, line
    matches); the J2-family path publishes its real name with
    nr_debug_entries = 0 (the documented deviation).
33. `publish_perfmap_uses_sanitized_real_name` — the perf-map line carries
    the sanitized real name (spaces to underscore).

## Deviations and deferred

- jitdump debug entries, the GDB JIT interface, managed sanitizers,
  deterministic replay, and real symbol names: **batch e, this batch —
  all landed** (the sections above are normative).
- Deterministic replay is the decision-record + first-divergence scope;
  exact re-execution scheduling stays out (no consumer demanded it; the
  differential checker is the deliverable with a named contract).
- `guest_tsan` is a reserved, named-refused policy bit until the atomics
  capability pack (post-M6) — enabling it never silently no-ops.
- GDB symfiles are ET_EXEC images with absolute symbol addresses (the
  reader-side contract gdb requires); they are not relocatable objects —
  a linker-consumable ET_REL form stays out until a consumer asks.
- The FFI boundary validates the RAW (unresolved) pointer base — what the
  emitted stub actually passes — so a handle to an evacuated (Brooks-
  resolvable) object is a NAMED boundary refusal, not transparently
  resolved. The strict-refusal semantics are the documented contract;
  a Brooks-resolving boundary form rides the call-out wiring batch.
- `validate_ptr_args` has no production call site yet (the trampolines'
  consumer contract is the driver-level entry); the wiring rides the
  interop/J4 call-out batch, which owns the real call sites.
- Safepoint-log events for preemptive (guard-page) suspensions: the
  emitted-poll batch (R31).
- Allocation-site attribution in samples: the emitted-poll batch (R31) —
  `site_id` reserved at 0 until the unbiased stack walk exists.
- `vx stats` prints the safepoint summary as the zero-valued numeric shape
  with an explanatory note — a cold driver run has no handshake manager,
  so no events can exist; the numbers become real under freeze_all and
  handshake consumers.
- TTSP/body boundaries are stamped at the manager's lock (re)acquisition
  (sub-microsecond over-attribution under contention is documented slop;
  the stamped-from-ack refinement rides the R31 batch with the emitted
  polls).
