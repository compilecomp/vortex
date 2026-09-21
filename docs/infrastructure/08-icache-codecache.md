# Infra 8 — Executable Memory & I-Cache Manager

Managing the physical reality of the CPU's instruction cache.

## Mechanisms

- **I-cache synchronization.** On ARM/RISC-V, modifying code requires explicit
  `DC CVAU` (data cache clean) and `IC IVAU` (instruction cache invalidate)
  instructions. The system batches these to avoid pipeline flushes.
- **NUMA-aware code placement.** Allocate J4 machine code on the NUMA node local
  to the thread executing it, preventing cross-node memory latency for instruction
  fetches.
- **Huge page code caching.** Request 2MB/1GB transparent huge pages (THP) from
  the OS for the code cache to minimize TLB misses during heavy instruction
  fetching.
- **Code sweeping & defragmentation.** Background threads identify cold J2/J3
  regions, unmap them, and compact the remaining hot code to improve I-cache
  locality.

## Vortex API

`include/vortex/infra/code_cache.hpp`:

- `CodeCacheSegment` — segmented store: interpreter handlers, baseline code,
  optimized code, outlined regions, stubs, barriers, deopt trampolines,
  continuation stubs.
- `CodeAllocation` — allocation handle with tier/kind/origin metadata.
- `IcSyncPolicy` — per-arch cache maintenance batching (x86-64 is a no-op beyond
  serialization; ARM64 hooks defined).
- `NumaCodePlacer`, `HugePagePolicy` — placement configuration.
- `CodeSweeper` — background cold-region identification and compaction contract.

Segment allocation with tier separation and metadata attachment is implemented and
unit tested; sweeping/compaction is contract-stubbed.
