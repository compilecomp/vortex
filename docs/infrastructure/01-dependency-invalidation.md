# Infra 1 — Dependency & Invalidation System ("CHA Engine")

Speculative optimization (J3/J4) relies on assumptions — "Class X has no
subclasses", "Method Y is final", "Shape Z is stable". When the guest language
loads new code or modifies shapes, those assumptions break. This system detects
the breakage and repairs exactly what is affected.

## Mechanisms

- **Dependency graph.** A directed graph mapping assumptions
  (`KlassLeaf(X)`, `MethodFinal(Y)`, `ShapeStable(Z)`, ...) to dependent regions
  (specific RBPD regions in J3/J4 code).
- **Invalidation broadcast.** When a class loads or a shape transitions, the
  runtime queries the dependency graph for affected assumptions.
- **Targeted atomic patching.** Instead of throwing away the whole method, the
  system atomically patches the entry of the specific failing RBPD region —
  overwrite a `JMP` with a `TRAP` or redirect to a deopt trampoline.
- **Lazy invalidation.** If a region is cold, invalidation is deferred until it is
  next executed.
- **Metadata garbage collection.** Unloading classes/modules must safely sweep the
  dependency graph and reclaim associated JIT metadata and code-cache regions.

## Vortex API

`include/vortex/infra/dependency.hpp`:

- `Assumption`, `AssumptionKind` (klass leaf, method final, shape stable, ...)
- `DependentRegionRef` — weak handle to an RBPD region
- `DependencyGraph` — `register_assumption`, `invalidate_klass`,
  `invalidate_method`, `invalidate_shape`, sweep on unload
- `InvalidationBatch` — collects affected regions for one atomic patching pass

The implementation (`src/infra/dependency.cpp`) is functional: assumption
registration, direct per-assumption invalidation, lazy dispatch, and sweep are
real and unit tested. Transitive invalidation across subclass/shape chains lands
with the class-hierarchy metadata (M6); native-code patching integrates with the
code cache (infra 8).
