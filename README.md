# Vortex

**The ultimate JIT: four optimizing tiers, a speculative register interpreter, and the
full runtime infrastructure around them — in C++26.**

> **Vortex is an engine, not a language.** It ships no source language, no syntax,
> no standard library of its own. Any guest language — static, dynamic, class-
> based, prototype-based, functional, scripting — ports into Vortex by emitting
> **Universal Guest Bytecode (UGB)** and declaring its capabilities and runtime
> hooks. See [docs/porting.md](docs/porting.md).

```
T0  speculative register interpreter      [not counted as a JIT tier]
J1  no-IR stencil baseline JIT            [escape the interpreter]
J2  fast optimizing JIT (light SoN)       [remove the first cliff]
J3  adaptive full optimizing JIT          [SoN + CIOG + RBPD, budgeted]
J4  max deterministic optimizing JIT      [no budget, no search, fixed point]
```

Vortex is a production-grade just-in-time compilation stack for a managed guest
language. It is built around the **Universal Guest Bytecode (UGB)** — a register,
capability-based bytecode with speculative typed forms — and every layer of a real
virtual machine: adaptive interpretation, four JIT tiers, region-based partial
deoptimization (**RBPD**), an incremental concurrent generational garbage collector
(**ICGGC**), and the nine infrastructure systems that make a JIT shippable
(dependency invalidation, W^X security, snapshots/AOT, M:N threading, FFI,
observability, CPU dispatch, I-cache/code-cache management, power awareness).

> **Status: M0 — spec-first.** The complete design specification, the full C++26 API
> surface, the UGB toolchain (text assembler, encoder/decoder, disassembler), the
> reference frontend (a minimal example of a language port), the working T0
> interpreter, the test harness, and CI are in place. Tier
> code generators (J1–J4), the concurrent GC engine, and the heavyweight infra
> backends are contract-stubbed and scheduled on the
> [roadmap](docs/roadmap.md).

---

## Quick start

```bash
# Requirements: GCC 14+ (or Clang 18+) with C++26, CMake >= 3.28, Ninja or Make
cmake --preset release
cmake --build --preset release -j

cmake --build --preset release -t test  # or: ctest --preset release

# Run a hand-written UGB program through the T0 interpreter
./build/release/bin/vx run examples/fib.ugb --entry fib --args 25

# Compile a program with the REFERENCE frontend (a minimal example of what a
# language port looks like — not a product language) and execute it
./build/release/bin/vx run examples/fib.mini --entry main

# Disassemble / inspect a UGB module
./build/release/bin/vx dis examples/fib.ugb
./build/release/bin/vx stats examples/fib.ugb
```

## The tier stack

| Tier | Kind | Input → Output | Compile budget | Purpose |
|------|------|----------------|----------------|---------|
| **T0** | speculative register interpreter | UGB → execution | none | startup, profiling, adaptive rewriting, ICs, OSR source |
| **J1** | stencil baseline JIT | UGB → machine code (no IR) | near-instant | escape the interpreter; stencils + superstencils |
| **J2** | fast optimizing JIT | UGB + profiles → light SoN → code | small, capped | remove the first performance cliff |
| **J3** | adaptive full optimizing JIT | UGB + profiles → full SoN/CIOG → code | budgeted, aggressive | main production tier; PEA, RBPD, vectorization |
| **J4** | max deterministic optimizing JIT | persistent IR → code | **no budget, no search** | peak quality for very hot stable code; deterministic fixed-point pipeline |

Tiering is driven by a deterministic policy (`include/vortex/runtime/tiering.hpp`):
invocation + loop backedge counters promote methods T0 → J1 → J2 → J3 → J4, OSR
moves hot loop bodies into higher tiers mid-execution, and RBPD deoptimization demotes
**only the failing region**, never the whole method.

## Repository layout

```
docs/                    design specification (start with docs/architecture.md, then docs/porting.md)
include/vortex/          the complete public C++26 API surface
  support/               arenas, tagged values, results, hashing, logging
  runtime/               object model, tagged heap, handles, tiering policy
  ugb/                   Universal Guest Bytecode: ISA, module codec, assembler
  vm/                    T0 speculative register interpreter
  ir/                    Sea-of-Nodes graph + CIOG
  j1/  j2/  j3/  j4/     the four JIT tiers
  deopt/                 region-based partial deoptimization (RBPD)
  gc/                    incremental concurrent generational GC (ICGGC)
  codegen/               abstract codegen + x86-64 assembler
  infra/                 the nine infrastructure systems
  frontends/reference/   Mini — a minimal EXAMPLE of a language port (test fixture)
                         (headers under include/vortex/frontends/reference,
                          implementation under src/frontends/reference)
src/                     implementations (spec-first: core path real, rest contracted stubs)
tools/vx/                the `vx` driver: assemble, run, dis, stats
tests/                   harness + unit/integration/golden tests
examples/                .ugb assembly and .mini reference-frontend demo programs
```

## The nine infrastructure systems

| # | System | Header |
|---|--------|--------|
| 1 | Dependency & invalidation ("CHA engine") | `include/vortex/infra/dependency.hpp` |
| 2 | Security & exploit mitigation (W^X, blinding, CFI, PAC, Spectre) | `include/vortex/infra/security.hpp` |
| 3 | Startup snapshots & AOT pipeline | `include/vortex/infra/snapshot.hpp` |
| 4 | M:N threading, handshakes, safepoints | `include/vortex/infra/threading.hpp` |
| 5 | Native interop (FFI) & ABI translation | `include/vortex/infra/ffi.hpp` |
| 6 | Observability, debugging & tooling | `include/vortex/infra/observability.hpp` |
| 7 | Hardware intrinsics & CPU dispatch | `include/vortex/infra/cpu_dispatch.hpp` |
| 8 | Executable memory & I-cache manager | `include/vortex/infra/code_cache.hpp` |
| 9 | Power & thermal management | `include/vortex/infra/power.hpp` |

Each system has a dedicated chapter under `docs/infrastructure/`.

## Design rules (the "rules to the letter")

1. **Four JIT tiers**, T0 is an interpreter and never counted as a tier.
2. **J4 has no artificial compile budget and no search-based compilation.** It runs a
   deterministic pipeline to fixed point or legal completion — never a superoptimizer.
3. **Every tier is enterable and exitable.** OSR up, RBPD down; deopt never discards
   the whole method, only the failing region.
4. **Speculation is always profile-directed and always repairable.** Guards are
   recorded, regions are scoped, fallbacks are close.
5. **UGB is a register bytecode** with speculative typed forms and generic fallback
   forms; the interpreter rewrites bytecode adaptively and atomically.
6. **W^X is absolute**: code memory is never writable and executable at the same time.
7. **The GC is incremental, concurrent, and generational**, with SATB barriers, Brooks
   forwarding, TLABs, and card marking integrated into every tier including deopt.

## Compiler laws & compliance

The repository is governed by the **UGB/VORTEX Compiler Laws & Architecture
Specification** (137 rules). Compliance is not narrative — it is mapped,
enforced and tested:

- `docs/compliance_matrix.md` — every rule mapped to its enforcement
  mechanism and status (Rule 134).
- `tools/lint/compliance.sh` + the CI `compliance` job — mechanical checks
  for the structural laws: no native exceptions (R65), no node-based
  containers on hot paths (R50), no raw-pointer IR edges (R48), no RTTI
  (R68), no shared_ptr/function in hot IR (R69).
- `docs/exception-register.md` — every deviation is registered with an
  expiry (Rule 132). Silent bypasses are violations.
- `docs/adr/` — architecture decision records for the dispatch design, the
  error-propagation policy, IR index edges, capability negotiation and the
  numeric semantics contract (Rule 135).
- `docs/guest-semantics.md` — the registered semantic oracle for the
  reference language, including the exact numeric behavior (Rule 106/110):
  overflow traps, IEEE-exact float ops, saturating float-to-int conversion.

## CEM-26 performance standard

Hot code additionally follows **CEM-26 Rev 1.1** (Cycle-Exact Maintainable
C++26). Performance is treated as a semantic property: every hot function
carries a `PERF_CONTRACT` (budget/cycles, memory traffic, branches, cache
behavior) plus a `PERF_OBSERVATION` record; hot/warm/cold classification is
explicit; every hot struct asserts its layout; exceptions from the standard
are `PERF_PERMIT`-granted and registered.

- `docs/cem26.md` — scope, cost-block conventions, layout audits, the live
  PERF_PERMIT register and the validation plan.
- `include/vortex/support/cem.hpp` + `include/vortex/ugb/encoding.hpp` —
  the semantic-domain constant registries (magic-number ban, section 2).
- `tools/lint/compliance.sh` — mechanical CEM-26 subset: no seq_cst in hot
  trees (s14), no mutexes (s7/14), `@hot` files must carry cost blocks
  (s3/4), permits must be registered (s17).

Build with compliance checks:

```sh
cmake --build build/release --target compliance_lint   # structural laws + CEM-26
ctest --test-dir build/release                          # 73-test suite
```

## License

MIT — see [LICENSE](LICENSE).
