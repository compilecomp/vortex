# Infra 9 — Power & Thermal Management System

Crucial for mobile/laptop deployments.

## Mechanisms

Hook into OS thermal APIs. If the device is overheating or on battery, the system
dynamically:

- suspends J4 compilation entirely;
- downgrades J3 compile budgets;
- reduces the M:N green thread worker pool;
- disables aggressive hardware prefetching.

## Vortex API

`include/vortex/infra/power.hpp`:

- `PowerState` — normal, battery-saver, thermal-warm, thermal-critical.
- `ThermalListener` — OS thermal status source (polling adapter contract).
- `PowerGovernor` — applies a power state to the runtime: J4 queue gate, J3
  budget multiplier, worker pool resize, prefetch policy.
- `CompileBudgetMultiplier` — computed budget scaling consumed by the tiering
  policy.
