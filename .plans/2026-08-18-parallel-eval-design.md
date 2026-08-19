# Parallel Evaluation for the Lix homelab fork — design doc

Date: 2026-08-18
Status: Stage 1 ✅ committed (c3306449f), Stage 2 ✅ committed (see below)
Base: `homelab/2.96` rebased onto upstream `main` (`3470ce4d5`), 15 homelab commits reapplied.

## 1. Goal

Bring Determinate Nix–style **parallel evaluation** to the homelab Lix fork so `nix flake check`,
`nix eval`, and `nix search` on the fleet's large flakes (quill 14 packages, 175 MCP tools;
nixos-config 5-host colmena) use all idle cores instead of one.

Measured baseline (bench/ harness, this morning, on the 32-core box):

| workload | eval-cores=1 wall time |
|---|---|
| host toplevel eval | 12–20 s per host |
| `nix flake check` | 16–33 s |

Target: >2× on flake check/eval with `eval-cores = 0` (auto).

## 2. What Determinate Nix actually did (verified against `DeterminateSystems/nix-src`)

- **44 commits over 2+ years**, all in libexpr. Three pillars:
  1. **Value representation surgery**: `Value` became two dwords `p0` (atomic) / `p1`, with
     discriminators encoding state. `p1` (payload) is always written before `p0`; `p0` is
     published with `release` and read with `acquire` via `exchange`/CAS. States:
     `thunk/app → pending → awaited → resolved`. A thunk being evaluated by thread A is
     "pending"; thread B forcing it CASes pending→awaited and blocks in `waitOnThunk()`
     (futex-based waiter list). When A finishes it `exchange`s p0 with the result pointer;
     if the old state was `awaited`, it wakes the waiters (`notifyWaiters()`).
  2. **Executor**: a thread pool (`boost::thread`) with a priority `multimap` work queue,
     `MoveOnlyFunction` work items, `spawn()` returning `std::future`s, `FutureVector`
     (fire-and-forget batch), an `eval-cores` setting (`0` = hardware_concurrency), and an
     interrupt callback. `thread_local amWorkerThread` gates re-entrant spawning.
  3. **Parallel primitives + wiring**: `builtins.parallel` primop, `parallelForceDeep`
     (used by flake check/show/search and `--json` value printing), plus a thread-safe
     SymbolTable.
- The parallelism surface is **narrow**: executor + `builtins.parallel` +
  `parallelForceDeep` + thunk-waiting. The deep Value rewrite exists to make thunk state
  transitions race-free, not because evaluation itself needs it.

## 3. Why the Lix port is much smaller than a from-scratch port

Lix's `Value` (as of the rebased fork) is **already a single packed pointer**:
`uintptr_t raw` with 3 tag bits (`tThunk=0`…`tAuxiliary`). Laziness is expressed through a
**separate GC-allocated `Value::Thunk` object**:

```cpp
struct alignas(Value::TAG_ALIGN) Value::Thunk {
    union { Env * _env; Value _result; };
    Expr * expr;
    bool resolved() const { return expr == nullptr; }   // memoized result in _result
    void resolve(Value v) { _result = v; expr = nullptr; }
};
```

Current `forceValue` thunk path (eval-inline.hh:193):

```cpp
if (v.isThunk()) {
    auto & thunk = v.thunk();
    if (thunk.resolved()) { v = thunk.result(); }
    else {
        const auto backup = thunk;              // exception rollback
        Env * env = thunk.env(); Expr & expr = *thunk.expr;
        thunk = Value::blackHole;               // "being evaluated" marker (replaces ptr!)
        try { v = expr.eval(*this, *env); thunk.resolve(v); }
        catch (...) { thunk = backup; tryFixupBlackHolePos(v, pos); throw; }
    }
}
```

The black hole is a static `Thunk` whose `expr` is a sentinel; re-forcing it raises
`InfiniteRecursionError`. **The single-threaded flaw**: while evaluating, the `Value` slot's
thunk pointer is *replaced* by `&blackHole`, so a second thread cannot find the original
thunk to wait on, and cannot distinguish "another thread is evaluating" from "infinite
recursion".

**Port thesis**: keep the packed-pointer `Value` and the separate `Thunk` object; move the
state machine *into the Thunk* (atomic fields), and change the force protocol from
"replace slot with blackHole" to "CAS thunk state in place". This is the minimal adaptation
of Determinate's design to Lix's representation.

## 4. Design

### Stage 1 — Executor infrastructure (additive, no Value surgery)

Deliverable: `lix/libexpr/parallel-eval.hh` + `.cc` (mirrors Determinate, adapted to Lix):

- `Executor` — thread pool + priority queue of `MoveOnlyFunction<void()>` work; `spawn()`
  → `std::vector<std::future<void>>`; `FutureVector` batch type; `thread_local amWorkerThread`.
- `eval-cores` setting in `EvalSettings` (`0` default → `hardware_concurrency`, clamped ≥ 1).
- **Threads must be registered with Boehm GC** (`GC_register_my_thread` /
  `GC_allow_register_threads`) — Lix's existing kj `ThreadPool` never registers, so we do
  it explicitly in the worker entry; values are GC-allocated from worker threads.
- Uses `std::thread` (Lix has no boost::thread; `MoveOnlyFunction` is a ~30-line shim in
  libutil or local to the executor header).
- Not wired into the evaluator yet; builds and unit-tests standalone.

### Stage 2 — Atomic thunk state machine (DONE, committed)

Implemented exactly as designed below, with one refinement: same-thread recursion is
detected via the **evaluating thread's id stored in the state word** (bits 2+), mirroring
Determinate's `threadId` in `p0` — no separate thread-local guard set is needed. The
`eBlackHole` marker is still written while evaluating so `isBlackhole()` and error messages
behave as before.

```cpp
enum class ThunkState : uint32_t { Unevaluated = 0, Evaluating = 1, Awaited = 2, Resolved = 3 };
constexpr uint32_t ThunkStateMask = 0x3;
constexpr uint32_t ThunkIdShift = 2;

struct Value::Thunk {
    union { Env * _env; Value _result; };
    Expr * expr;
    std::atomic<uint32_t> state;   // bits 0-1: ThunkState; bits 2+: evaluating thread id
};
```

Force protocol (per thread):

1. Read thunk state (acquire).
2. `Resolved` → copy `_result` (acquire).
3. `Unevaluated` → CAS to `Evaluating | (myId << 2)`; **winner** copies env/expr out,
   writes the `eBlackHole` marker, evaluates. On success `publish(v)` stores `_result`,
   nulls `expr`, exchanges state → `Resolved` (release), and wakes waiters iff the
   exchange returned an `Awaited` state. On exception: restores env/expr, exchanges
   state → `Unevaluated` (release), wakes waiters iff it was `Awaited`, rethrows.
4. `Evaluating`/`Awaited` → if the stored id == my id: `InfiniteRecursionError` (same-thread
   recursion). Else CAS → `Awaited | (id << 2)` (weak), then `waitOnThunk()` blocks on a
   sharded waiter-domain cv until the state leaves `Awaited`; loop.

Waiter domains: `std::array<Sync<WaiterDomain>, 128>` keyed by thunk address (`>> 5 % 128`),
exactly like Determinate. `wakeAllThunkWaiters()` is invoked by the Executor's interrupt
callback so blocked waiters unwind on SIGINT. New files: `lix/libexpr/thunk-wait.{hh,cc}`.

App resolution (`Value::App`, `_n == ~0` = resolved): left single-threaded in Stage 2.

Slot publication: the winner writes its result into its own `Value & v` slot; waiters never
read the slot after observing `Evaluating` — they read `Thunk::_result` (acquire) and copy
it into their slot. The slot write is pointer-sized and idempotent.

Known limitation (same as Determinate): a **cross-thread thunk cycle** (thread A waits on
B's thunk while B waits on A's) deadlocks — thread-id detection only catches same-thread
cycles. Pathological Nix only; accepted for now.

Measured: no single-threaded regression. A/B of the zephyr toplevel eval (Stage 1 binary
vs Stage 2 binary, same build config, alternating under load): median 25.6 s vs 25.5 s —
the atomic machinery is effectively free on this workload.

### Stage 3 — Parallel primitives + wiring

- `builtins.parallel` primop: map a function over a list, one future per element via the
  Executor, collect in order (result deterministic — evaluation is pure).
- `parallelForceDeep` for `forceValueDeep` call sites: flake check attr evaluation, flake
  show/search, `nix eval --json` value printing.
- `nix flake check`: evaluate each check attrset in parallel.
- Keep `eval-cores=1` as the escape hatch; default `0` (auto).

### Stage 4 — Thread-safe EvalState internals

- **SymbolTable**: interning must be safe from worker threads (symbols are added during
  parse/eval). Shard with per-bucket mutexes or a concurrent hash map; symbol IDs are
  stable ints so the rest of the code is unaffected.
- **EvalMemory small-object caches** (`gcCache[8]`, `stats`): per-thread caches (Boehm's
  `GC_MALLOC` is itself thread-safe; the *cache* is the race). Make caches thread_local and
  keep the stats counters atomic.
- **EvalState caches** (`fileEval`, `resolvedPaths`, primop registration): audit for
  write-once/read-many; guard the mutating paths.

## 5. GC threading (critical, do first)

- Boehm GC is thread-safe, but **only for registered threads**. Worker threads must call
  `GC_register_my_thread` (with `GC_allow_register_threads` at init) before any
  `LIX_GC_MALLOC*` and `GC_unregister_my_thread` on exit.
- The executor must create its worker threads from the main thread (or a registered one).
- Verify with a GC stress test (the existing gc tests) under `eval-cores=8`.

## 6. Errors, interrupts, determinism

- Exceptions (eval errors) inside worker work items must propagate to the awaiting thread;
  `std::future` already does this — the spawn/finishAll layer rethrows on `get()`.
- First error wins: `FutureVector::finishAll()` should stop scheduling new work after the
  first failure (mirror Determinate), preserving error messages/ordering for single errors.
- Interrupt (SIGINT): executor checks the interrupt flag between work items; the
  `InterruptCallback` from Determinate is reimplemented on Lix's existing interrupt
  machinery.
- Determinism: evaluation is pure; parallel scheduling changes only timing, not results.

## 7. Verification

- Existing `bench/` harness (baseline captured this morning): compare `eval-cores=1` vs
  `eval-cores=0` on host toplevel eval + `nix flake check` of nixos-config/quill.
- `nix flake check` of the lix repo itself + unit tests (`meson test`).
- A new unit test: `builtins.parallel` on a list of thunks forces correct results in order.
- GC stress: eval under `eval-cores=8` with Boehm verify mode if available.

## 8. Risks

| risk | mitigation |
|---|---|
| Boehm + unregistered threads → heap corruption | explicit register/unregister in worker entry; stress test |
| Data race on `Value` slots (UB by the standard, benign in practice) | waiters only read thunk state/result (acquire); slot write is idempotent pointer copy |
| Error-ordered nondeterminism under concurrency | first-error-wins via FutureVector; document remaining divergence |
| Regressing single-threaded eval perf | all machinery gated on `eval-cores > 1`; thunk layout kept cache-friendly (state byte inside existing struct) |
| `eval-explorations` (Lix's own Value-modernization branch) conflicts | branch is stale (Nov 2025, 1103 ahead/30 behind) and has no atomic thunk work; we build on rebased main |

## 9. References

- `DeterminateSystems/nix-src` (cloned at `/tmp/dnix`): `src/libexpr/include/nix/expr/parallel-eval.hh`,
  `parallel-eval.cc`, `value.hh` (p0/p1 + finish()/notifyWaiters), `eval-settings` (`eval-cores`).
- Lix fork: `lix/libexpr/value.hh` (packed Value + Thunk), `eval-inline.hh` (forceValue),
  `eval.hh` (EvalMemory), `lix/libutil/thread-pool.cc` (existing kj pool — not GC-registered).
- nixpkgs: quickshell unrelated; this doc is eval-only.
