# Porting a Language to Vortex

Vortex is an **engine, not a language**. It has no source syntax, no standard
library, and no opinion about your semantics. A language port is a frontend that:

1. lowers your language's semantics into **Universal Guest Bytecode (UGB)**,
2. declares the **capabilities** it requires,
3. provides **runtime hooks** for the semantics UGB deliberately does not hardcode,
4. states a **conformance level** it relies on.

Once those four things exist, the entire stack — T0 interpretation, tiering, the
four JIT tiers, RBPD, the ICGGC, and the infrastructure systems — works on your
language without modification.

```text
your language frontend          vortex engine
+--------------------+         +---------------------------------------+
| lexer / parser     |         | verifier -> T0 -> J1 -> J2 -> J3 -> J4|
| your type system   |  UGB    | RBPD, ICGGC, code cache, FFI, ...     |
| your overload rules|=======> | (the engine never sees your source)   |
| your runtime hooks |         +---------------------------------------+
+--------------------+
```

---

## 1. What UGB gives you, and what it leaves to you

UGB provides portable *mechanisms*, not language *policies*:

| Mechanism (UGB provides) | Policy (you provide) |
|---|---|
| `Add.Any` canonical arithmetic with site IDs | whether `+` wraps, traps, saturates, or promotes to bignum |
| Dispatch descriptors (`Call.Virtual`, `Call.Prototype`, `Call.Dynamic`, ...) | method resolution order, `method_missing`, multimethod ranking |
| Shape tokens + `GetProp.Dynamic` with IC fallback | prototype chains, `__getattr__`, proxies, metaclasses |
| `Closure.New` + capture descriptors | capture-by-value vs by-reference rules, lambda lifting |
| Exception table entries (`is_catch/is_finally/is_resumable`) | exception taxonomy and matching rules |
| `Check.*` guards with deopt records | what is checked, when, and with what fallback |
| `SuspendPoint` / coroutine ops | scheduler policy, async event loop |
| `Raw.*` / `Foreign.*` under `unsafe_memory`/`ffi` capabilities | borrow rules, memory safety policy (checked before UGB emission) |

The rule of thumb from `docs/ugb.md` section 10: **UGB is not a source AST, not a
universal type system, not a memory model, and not a scheduler.** Your runtime
remains responsible for the language-specific semantics; the engine optimizes and
executes the bytecode form.

---

## 2. Step-by-step port

### Step 1 — Lower semantics to UGB instructions

Emit methods as UGB method containers (`include/vortex/ugb/builder.hpp` exposes a
programmatic builder; the text assembler shows the format):

```text
.method add(regs=4, args=2)
    Add.I32 v2, v0, v1, site_1
    Return v2
.end
```

Use **canonical opcodes** (`Add.Any`, `GetField`) where semantics are dynamic, and
**specialized opcodes** (`Add.I32`, `GetField.Shape`) only where your frontend can
guarantee operand types, overflow behavior, and a fallback path. A specialized
opcode that fails at runtime *must* fall back to canonical semantics — that
contract is what makes speculation safe across all tiers.

### Step 2 — Declare capabilities

List what your code requires:

```text
ugb.pack.class_objects        # if you emit New.Object.Klass / Call.Virtual
ugb.pack.closures             # if you emit Closure.*
ugb.pack.exceptions           # if you emit Throw / exception tables
ugb.pack.atomics              # if you emit Atomic.* / Fence.*
...
```

The engine advertises what it supports. Unsupported capability -> the method falls
back to your runtime hook, is rejected, or runs through an interpreter hook — you
choose the policy per capability.

### Step 3 — Provide runtime hooks

Hooks are named natives your frontend registers with the engine
(`Interpreter::register_builtin` in M0; the FFI/hook table in later milestones):

```cpp
interp.register_builtin("mymath.add_hook", &my_add_hook, ctx);
interp.register_builtin("myrt.getprop_hook", &my_getprop_hook, ctx);
```

Canonical opcodes with unstable profiles route through these hooks (e.g.
`Add.Any` when operands are not both integers, `GetProp.Dynamic` when the shape
IC misses). Your hooks implement prototype lookup, operator overloading, coercion,
`method_missing`, property traps — whatever your language needs.

### Step 4 — State your conformance level

```text
UGB-Core-Interpreter   T0 can execute everything you emit
UGB-Baseline           J1 stencil coverage is sufficient for your hot paths
UGB-FastOpt            J2's light pipeline preserves your semantics
UGB-FullOpt            J3 speculation (PEA, inlining, RBPD) is sound for you
UGB-MaxOpt             J4 deterministic peak tier is sound for you
```

The levels differ in how much speculation the engine may apply to your code.
Guards make speculation safe by construction, but conformance levels let a
conservative frontend opt into less aggressive machinery while still running on
the same engine.

---

## 3. Tokens: how your object model maps in

UGB hardcodes no object model. You describe yours with tokens
(`docs/ugb.md` section 4.4):

- **Class-based OO**: emit `New.Object.Klass`, `Call.Virtual`, `GetField.Shape`,
  `Check.Type`. Provide exact type tokens and final/sealed facts where known —
  the engine then devirtualizes and inlines.
- **Prototype-based OO**: emit `GetProp.Dynamic` / `SetProp.Dynamic` +
  `ShapeToken`s. The engine builds hidden-class ICs and lowers hot sites to shape
  guard + offset load; misses route to your `getprop_hook`.
- **Dynamic scripting**: use `Call.Dynamic` with a `method_missing`-style hook;
  operator overloading flows through `Add.Any`/`Eq.Any` + hooks.
- **Functional languages**: `Closure.New` with capture descriptors,
  `TailCallDirect`, pattern matches lowered to `Switch.Int` + guards.
- **Systems languages**: `Raw.Load/Store`, `Pointer.*` under the `unsafe_memory`
  capability; borrow checking happens in your frontend, before UGB.
- **Async languages**: `Await` / `SuspendPoint` / coroutine ops; your runtime
  owns the event loop and scheduler policy.

The full coverage matrix lives in `docs/ugb.md` (section 1, "Coverage", and the
per-family mechanism table in section 10 of `docs/architecture.md`'s UGB summary).

---

## 4. The reference frontend ("Mini") is an example, nothing more

`frontends/reference/` contains **Mini**: a ~400-line expression language with
functions, control flow and calls, lowered to UGB. It exists only to:

- prove the port path end-to-end (source text -> UGB module -> verifier -> T0),
- serve as the executable documentation of Step 1,
- give CI a second frontdoor beyond hand-written `.ugb` assembly.

It is a test fixture. Do not treat Mini as "the Vortex language" — it is the
smallest thing that demonstrates a language port. Real ports look like your
language: your parser, your types, your hooks, your capability set.

---

## 5. What the engine promises in return

1. **Verifiable bytecode**: malformed or unsafe UGB is rejected before execution.
2. **All four tiers + T0** on the same module, with OSR up and RBPD down.
3. **Profile-driven optimization without source knowledge**: your site IDs feed
   the ICs, the tiering policy, and every optimizer pass.
4. **GC integration**: precise root maps and barriers are derived from your
   bytecode's reference operations, not from your source language.
5. **The nine infrastructure systems** apply uniformly: W^X, invalidation,
   snapshots, threading, FFI, observability, CPU dispatch, code cache, power.
