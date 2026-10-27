# Infra 6 — Observability, Debugging & Tooling System

You cannot ship a black box. The JIT must dynamically expose its internals to OS
tools. The M6-d batch lands the **observability floor** — the external
performance review's R40 ("ship the observability floor early... enterprise
adoption is gated more on this section than on any percentage"): the
`perf-jitdump` writer with its `perf-<pid>.map` sidecar, the safepoint TTSP
log, GC pause-ring cause labels, and the TLAB-refill allocation sampler. The
GDB JIT interface, managed sanitizers, and deterministic replay remain
contract-stubbed for batch e.

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
- `GdbJitInterface`, `SanitizerPolicy`, `ReplayRecorder` — unchanged
  contract stubs (batch e).

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

## Deviations and deferred

- jitdump debug entries (UGB source-line mapping), the GDB JIT interface,
  managed sanitizers, and deterministic replay: batch e.
- Safepoint-log events for preemptive (guard-page) suspensions: the
  emitted-poll batch (R31).
- Allocation-site attribution in samples: the emitted-poll batch (R31) —
  `site_id` reserved at 0 until the unbiased stack walk exists.
- Symbol names are synthetic (`vx_<tier>_m<id>`); real names ride the
  metadata interchange upgrade (batch e).
- `vx stats` prints the safepoint summary as the zero-valued numeric shape
  with an explanatory note — a cold driver run has no handshake manager,
  so no events can exist; the numbers become real under freeze_all and
  handshake consumers.
- TTSP/body boundaries are stamped at the manager's lock (re)acquisition
  (sub-microsecond over-attribution under contention is documented slop;
  the stamped-from-ack refinement rides the R31 batch with the emitted
  polls).
