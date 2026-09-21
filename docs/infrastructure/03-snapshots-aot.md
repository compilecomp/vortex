# Infra 3 — Startup Snapshots & AOT Pipeline

T0 and J1 are fast, but parsing and compiling core libraries on every startup is
wasteful. This system removes that cost.

## Mechanisms

- **Heap snapshots (V8-style).** A build-time tool runs the core library through
  J3, serializes the entire heap — objects, shapes, ICs, and compiled machine code
  — into a memory-mapped file. On startup the OS `mmap`s it directly into the
  runtime, skipping T0/J1 entirely for core code.
- **Shared code cache.** Cross-process sharing of J3/J4 compiled code for standard
  libraries using read-only shared memory segments (like .NET's CrossGen or V8's
  code cache).
- **Profile-Guided AOT (PGO-AOT).** An offline compiler ingests production runtime
  profiles (`.ic` and `.branch` files) and runs the J4 deterministic pipeline to
  emit a native shared library (`.so`/`.dll`), bypassing the JIT entirely for
  deployment.

## Vortex API

`include/vortex/infra/snapshot.hpp`:

- `SnapshotWriter` / `SnapshotReader` — heap + code serialization with
  relocatable pointers and verification hashes.
- `SnapshotFormat` — versioned container describing sections (objects, shapes,
  ICs, code, metadata).
- `SharedCodeCacheConfig` — read-only mapped segment configuration.
- `PgoProfileInput` / `AotPipeline` — profile ingestion and the J4 offline
  pipeline driver contract.

Contract-stubbed in this milestone; the API surface and serialization container
format are fixed so snapshot support can land without breaking the runtime.
