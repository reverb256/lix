# Optimization Brainstorm & Research — 2026-08-19

## Current State

### What's deployed (homelab build)
| Optimization | Status | Measured Impact |
|---|---|---|
| `-march=znver3 -O3` + LTO | ✅ | Baseline tuned build |
| `RUSTFLAGS -C target-cpu=znver3` | ✅ | — |
| Boehm GC 8GiB initial heap | ✅ | Reduces GC frequency |
| mimalloc | ✅ | Faster small allocations |
| Transparent huge pages `[always]` | ✅ | TLB efficiency |
| PGO (two-stage) | ✅ | **+2.5–3% eval speed** |
| Parallel eval executor | ✅ | Ready for JSON output |
| 60MB main thread stack | ✅ | Prevents -O3 stack overflow |

### What's NOT deployed
| Potential | Why not |
|---|---|
| PGO training on actual homelab config | Can't run `nixos-rebuild` in sandbox |
| `__builtin_expect` on hot paths | Micro-opt, needs profiling data |
| SIMD for string ops | Nix strings are variable-length; SIMD helps fixed-width |
| GC huge pages (explicit) | THP `[always]` already covers it |

---

## Upstream Status: 12 commits behind `origin/main`

The upstream has diverged significantly in structure — **parallel eval was removed entirely**. Our branch keeps it, which is correct for the homelab.

### Relevant upstream commits (since our fork point)

**Hash refactoring** (10 commits, by eldritch horrors):
- `90dcc2a72` — store hash data on heap (explicit: "no need to optimize for speed")
- `3b85d017b` — `Hash::to_base16` instead of `to_string`
- `989b1a4cc` — `Hash::to_base32` instead of `to_string`
- `2acff5057` — `Hash::to_sri` instead of `to_string`
- `3470ce4d5` — generic base16 encoder/decoder
- `12aa432b0` — `Hash` converts to `span<u8>` directly

**Store fix** (1 commit):
- `023478efb` — don't close invalidated connections too early (bugfix)

**Recommendation**: Pull these in. The hash refactoring is a clean-up (no perf impact for eval). The store fix is a bugfix.

---

## Optimization Opportunities (ranked by estimated impact)

### 1. PGO training on actual homelab config (estimated: 1–2% additional)
The current PGO training uses generic nixpkgs/NixOS workloads. The cluster evaluates the *same* config repeatedly. Training on the actual `nixos-config` rebuild would give clang better branch prediction data for the exact hot path.

**How**: In the `postInstall` of `pgoInstrumented`, evaluate the real config instead of a generic one:
```bash
LLVM_PROFILE_FILE=... nix eval --impure \
  --expr "let ec = import ${pkgs.path}/nixos/lib/eval-config.nix; \
    in builtins.length (builtins.attrNames \
      (ec { modules = [ /etc/nixos/configuration.nix ]; }) \
      .config.system.build.toplevel)"
```
**Problem**: The sandbox can't access `/etc/nixos/`. Would need to bake the config into the derivation.

### 2. `checkedArrayAllocSize` overhead (estimated: 0.5–1%)
Called 4.4M times. Each call constructs a `Checked<size_t>` and checks for overflow. For *most* callers, the size is known-safe (small, fixed-type arrays).

**How**: Add a `__builtin_expect` branch prediction hint, or a `[[gnu::const]]` fast path for sizes below a threshold:
```cpp
if (__builtin_expect(howMany < 1024 && size <= 8, 1))
    return howMany * size; // Fast path: small, no overflow risk
```

### 3. `PosIdx` construction overhead (estimated: 0.3–0.5%)
13M constructor calls. Each is just a `uint32_t` assignment, but the call count is massive. Some callers could use `noPos` as a default instead of constructing a real PosIdx.

**How**: Audit callers of `PosIdx(uint32_t)` for cases where `noPos` would suffice. Not a high priority.

### 4. `Value::internalType()` branch prediction (estimated: 0.2–0.3%)
45M calls. The hot path in `forceValue` checks `isThunk()` then `isApp()`. Adding `__builtin_expect` to favor the "already resolved" case could help:
```cpp
if (__builtin_expect(!v.isThunk() && !v.isApp(), 1))
    return; // Fast path: already resolved
```

### 5. `Bindings::get` for medium-sized attrsets (estimated: 0.2%)
Currently switches at 16 elements. Profile shows `Bindings::get` at 5.9M calls. For attrsets between 16–32 elements, the linear scan might still be faster due to branch prediction, but this needs measurement.

### 6. Dedup `internalType()` calls (estimated: 0.1–0.2%)
`Value::type()` calls `internalType()` multiple times in some paths. Caching the result in a local variable could save a few instructions per call.

---

## What's NOT worth optimizing (and why)

| Idea | Why not |
|---|---|
| SIMD for string comparison | Nix strings are variable-length; `memcmp` is already SIMD-optimized in glibc |
| `std::unordered_map` for Bindings | Attrsets are sorted arrays; binary search is cache-friendly |
| Custom allocator beyond mimalloc | Boehm GC handles allocation; mimalloc helps the non-GC path |
| Thread pool tuning | Our executor already uses a thread pool; pool size is hardware-bound |
| `GC_push_regs` tuning | Boehm's default register scanning is already optimal |

---

## Recommended Actions

### Immediate (this session)
1. **Pull in upstream 12 commits** — clean up, bugfix, no conflict risk
2. **Add `__builtin_expect` to `forceValue` hot path** — micro-opt, safe

### Short-term (next build cycle)
3. **Train PGO on actual homelab config** — requires baking config into the derivation
4. **Audit `checkedArrayAllocSize` callers** — find safe fast paths

### Medium-term (future)
5. **Profile with `perf record` on real rebuild** — find actual micro-arch bottlenecks (cache misses, branch mispredictions)
6. **Consider `-fomit-frame-pointer`** — frees a register on x86-64 (already default with `-O3`, but verify)

---

## Questions for the user

1. Should we pull in the upstream 12 commits now?
2. Should we attempt PGO training on the actual homelab config (requires baking the config into the derivation)?
3. Should we add `__builtin_expect` annotations to the hot path?
