# Lix — Project Knowledge

## What this is

**Lix** is a fork of **Nix** (CppNix): a purely functional package manager for Linux/macOS/BSD. Repo is the Lix implementation, built with **Meson** (C++23) + a Rust workspace. Also vendors `nix-eval-jobs` as a subproject.

This checkout is the **homelab fork** on branch `homelab/2.96` (rebased onto upstream `main`), carrying local feature commits (parallel evaluation) on top of the homelab/CI changes. Unlike upstream Lix (Gerrit), this fork commits directly on the branch — no `refs/for/main` needed.

## Where key code lives

- `lix/libutil/` — foundation library (config/settings, args, errors, async, hashing, logging, strings, serialisation). Also `lix/libutil/experimental-features/`, `lix/libutil/deprecated-features/` (data-file-driven feature definitions).
- `lix/libstore/` — store implementation, database (sqlite), binary caches, remote stores, GC, sandboxing; `schema.sql`.
- `lix/libfetchers/` — fetching inputs (git, tarballs, flakes).
- `lix/libexpr/` — Nix language evaluator. Builtins live in `lix/libexpr/builtins/`, builtin constants in `lix/libexpr/builtin-constants/` (data files registered in `lix/libexpr/meson.build`).
- `lix/libmain/`, `lix/libcmd/`, `lix/nix/` — the `nix` CLI command itself (`main.cc`, per-subcommand `*.cc`).
- `lix/lix-rs/` — Rust crates (zngur-based C++↔Rust bridge, `main.zng`), async sqlite, etc.
- `lix/lix-doc/` — Rust doc extraction used by libcmd.
- `tools/licxxbridge/` — Rust tool for the C++/Rust bridge.
- `tests/unit/` — gtest/rapidcheck unit tests (suite `check`), data under `tests/unit/<lib>/data/<lib>`.
- `tests/functional/` — bash functional tests (suite `installcheck`); `tests/functional2/` — newer pytest rewrite.
- `tests/nixos/` — NixOS VM integration tests (run via flake `hydraJobs.tests.*`).
- `pyproject.toml` — ruff config (linting/formatting for `tests/functional2/` Python code; requires Python >= 3.12). Not wired into `just`; run `ruff check`/`ruff format` manually.
- `doc/manual/` — user manual (mdbook); `doc/manual/rl-next/` — release notes for unreleased changes; `doc/manual/src/contributing/` — hacking guide.
- `meson.build` — top-level build logic (options, platform quirks, deps); `meson.options` — build options.

## Current work: parallel evaluation (homelab fork)

Goal: Determinate Nix–style parallel evaluation so `nix flake check`/`eval`/`search` use all cores. Design doc: `.plans/2026-08-18-parallel-eval-design.md`.

- **Stage 1** (c3306449f) — `lix/libexpr/parallel-eval.{hh,cc}`: `Executor` (thread pool + priority queue, `spawn()` → futures, `FutureVector`), `eval-cores` setting (`0` = auto → `min(32, cores)`; `1` disables). Worker threads must call `GC_register_my_thread`/`GC_unregister_my_thread` (Boehm).
- **Stage 2** (f587b55bf) — `lix/libexpr/thunk-wait.{hh,cc}` + `Value::Thunk` atomic state machine: `ThunkState {Unevaluated, Evaluating, Awaited, Resolved}` in an atomic word (bits 2+ = evaluating thread id); CAS-based force protocol in `eval-inline.hh`; 128 sharded waiter domains. Same-thread recursion → `InfiniteRecursionError`; cross-thread cycles deadlock (accepted).
- **Stage 3** (f002c0a1b) — `builtins.parallel` primop + `parallelForceDeep` wired into `nix eval --json` (`value-to-json.cc`) and `nix flake check`. Thread-safety pulled forward: `ChunkedVector` append-safe, `SymbolTable` intern mutex, thread_local `EvalMemory` caches/`callDepth`, atomic stats, `EvalState`-owned `FutureVector` drained in `~EvalState()`.
- **Stage 4** (open) — SymbolTable sharding, EvalMemory per-thread caches, EvalState cache audit.
- Experimental feature `parallel-eval` is **declared but not enforced** in C++ — parallelism engages whenever `eval-cores > 1` (the default), so `eval-cores = 1` is the escape hatch.
- Bench harness: `bench/eval-bench.sh` (results in `bench/results/`). Usage: `EVAL_CORES=0 ./bench/eval-bench.sh ./build/lix/nix/nix`. Runs 6 eval-bound workloads (flake check, 4 host toplevels, flake show) against `nixos-config`; `--no-eval-cache` so it times the evaluator. Last results: flake-show ~3× faster, toplevel evals ~20% slower (they don't hit the parallel paths), flake-check ~10% faster.

## Commands (build/test/lint)

Requires an existing Nix/Lix install for the dev shell (with `flakes` and `nix-command` experimental features enabled for `nix develop`); everything below runs inside the dev shell:

```bash
nix develop                                  # dev shell (also: nix develop ".#native-clangStdenvPackages")
just setup --wipe && just test               # clean build + install + full test suite (integration tests need install)
just setup && just test-unit                 # unit tests only (suite check)
just test-integration                        # functional tests only (suite installcheck)
just test-functional2 --collect-only         # pytest-based functional2 tests, extra args forwarded to pytest
just test-rs                                 # cargo tests via meson (colored output)
just build / just install / just clean   # clean also runs cargo clean (nukes the Rust target dir)
just lint                                    # clang-tidy + clippy (ninja -C build clang-tidy / clippy)
just test --list                             # list meson tests
meson test -C build --print-errorlogs --max-lines 10000   # manual, from justfile
```

Manual path: `meson setup ./build --prefix=$out $mesonFlags` → `meson compile -C build` → `meson test -C build --suite=check` → `meson install -C build` → `meson test -C build --suite=installcheck`. Build dir defaults to `./build`, install prefix to `./outputs/out`; override with `just builddir=... outdir=...`.

Release build: `nix build` (or `nix build .#nix-ccacheStdenv` for faster rebuilds). Cross builds: `nix build .#packages.<system>.default`, `nix build .#nix-armv6l-linux`, etc.

Documentation: `meson compile -C build manual` (broken internal links fail the build); API docs via `meson configure build -Dinternal-api-docs=enabled`.

## Conventions & gotchas

- **Code review is Gerrit** upstream (`git push origin HEAD:refs/for/main` to `ssh://USER@gerrit.lix.systems:2022/lix`), not GitHub PRs — though small PRs on the GitHub mirror are accepted (~300 lines). **The homelab fork commits directly on `homelab/2.96`.**
- **User-visible changes need a release note** in `doc/manual/rl-next/` (YAML front-matter: `synopsis`, `issues` (fj# = Forgejo), `prs`, `cls`, `category`, optional `credits`/`significance`; add `significance: significant` to promote). Categories: Breaking Changes, Features, Improvements, Fixes, Packaging, Development, Miscellany. Registered in `doc/manual/change-authors.yml`.
- **C++23, coroutine-heavy async** (`LIX_TRY_AWAIT`, generators). GCC is known to miscompile coroutines (>=13 allowed, crashes expected); **clang is the preferred compiler**. PCH enabled by default (`lix/pch/precompiled-headers.hh`).
- **Rust/clippy lints are denied** (workspace lints incl. `unwrap-used`, `todo`, `rc-mutex`, `clone-on-ref-ptr`).
- **Rust is bridged to C++ via zngur** (`*.zng` files); pinned git rev in `Cargo.toml`.
- **Characterization tests** (golden output): regenerate with `_NIX_TEST_ACCEPT=1` (unit and functional).
- **Meson quirks**: `--reconfigure`/`--wipe` preserve previously-set `-D` options; new/changed defaults in `meson.options` aren't propagated — use `just clean` for truly clean builds. `meson introspect` shows buildsystem state.
- Settings/experimental/deprecated features are data files with YAML front-matter, not C++ enums — new ones must be registered in the relevant `meson.build` `*_definitions` list.
- UBSan: `-fsanitize=signed-integer-overflow -fsanitize-undefined-trap-on-error` are added by default, but only when `b_sanitize` does not include `undefined` (i.e. dev UBSan replaces them) and the compiler is gcc/clang.
- `just` positional-arg forwarding: targets that accept args can't be chained (`just build test` ≠ `just build && just test`); use `-j4` style args before chaining.
- Store/lib names: libraries are `liblixutil`, `liblixstore`, `liblixexpr`, etc. (confirmed in `tests/unit`); headers under `lix/` namespace.
- Editor integration: `clangd` ships in the dev shell; Meson generates `build/compile_commands.json` — symlink it to the repo root (`ln -sf ./build/compile_commands.json ./compile_commands.json`) for LSP.
- Project is the "lix" implementation of Nix; upstream CppNix bugs are mirrored in the issue tracker (organised per wiki).
