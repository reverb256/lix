#!/usr/bin/env bash
# Evaluation regression gate.
#
# Fails (non-zero) if NEW is more than THRESHOLD% slower than BASE on the
# evaluation-bound "rebuild" workload (a full NixOS toplevel eval). Both
# builds are measured in single-threaded mode (`--option eval-cores 1`)
# so the comparison is about basic evaluator performance, not the parallel
# machinery (which is still opt-in and workload-dependent).
#
# Usage:
#   ./bench/regression-gate.sh <base-dir-with-bin/nix> <new-dir-with-bin/nix>
#   THRESHOLD=10 RUNS=5 ./bench/regression-gate.sh result-base result-new
#
# Environment:
#   THRESHOLD  max % slowdown before failing (default 10)
#   RUNS       timed runs per build, interleaved (default 5)
#
# Deliberately self-contained: uses coreutils only (no hyperfine), pins to
# two CPUs with taskset, and uses chrt realtime priority if the caller has
# permission (degrading gracefully otherwise).

set -euo pipefail

BASE="${1:?usage: regression-gate.sh <base-dir-with-bin/nix> <new-dir-with-bin/nix>}"
NEW="${2:?usage: regression-gate.sh <base-dir-with-bin/nix> <new-dir-with-bin/nix>}"
THRESHOLD="${THRESHOLD:-10}"
RUNS="${RUNS:-5}"

for d in "$BASE" "$NEW"; do
    if [ ! -x "$d/bin/nix" ]; then
        echo "FATAL: $d/bin/nix is not an executable" >&2
        exit 2
    fi
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

# Evaluation-bound workload: the NixOS installation-CD toplevel. Matches
# the `rebuild` case in bench/bench.py.
EXPR='(import <nixpkgs/nixos> { configuration = ./bench/nixpkgs/nixos/modules/installer/cd-dvd/installation-cd-graphical-calamares-plasma6.nix; }).config.system.build.toplevel'

# Pinned local nixpkgs checkout (shared with bench/bench.py); needs the
# flake's locked nixpkgs input, so build it once if absent.
if [ ! -e bench/nixpkgs ]; then
    nix build --extra-experimental-features 'nix-command flakes' --impure \
        --expr '(builtins.getFlake "git+file:.").inputs.nixpkgs.outPath' -o bench/nixpkgs
fi

TMP="$(mktemp -d)"
# Store paths are chmod'd read-only, so make them writable before removal.
trap 'chmod -R u+w "$TMP" 2>/dev/null || true; rm -rf "$TMP"' EXIT
export NIX_CONF_DIR=/var/empty
export NIX_REMOTE="$TMP"
export NIX_PATH="nixpkgs=$REPO/bench/nixpkgs"

# Pin to two dedicated CPUs; add realtime priority when permitted. This is
# the same protocol as bench/bench.py and keeps the comparison stable even
# on a box that is also doing the CI build.
PIN=(taskset -c 2,3)
RT=()
if chrt -f 50 true 2>/dev/null; then
    RT=(chrt -f 50)
fi

nanos() { date +%s%N; }

median() {
    sort -g | awk '{ a[NR] = $1 } END { print (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2 }'
}

run_once() {
    local bin="$1"
    # Only pass eval-cores to binaries that support it (a pre-parallel
    # baseline binary has no such setting).
    local flags=()
    if "$bin" show-config 2>/dev/null | grep -q '^eval-cores'; then
        flags+=(--option eval-cores 1)
    fi
    local t0 t1
    t0="$(nanos)"
    "${PIN[@]}" "${RT[@]}" timeout 300 "$bin" \
        --extra-experimental-features 'nix-command flakes' \
        "${flags[@]}" eval --raw --impure --expr "$EXPR" >/dev/null 2>&1
    t1="$(nanos)"
    echo $((t1 - t0))
}

# Warm the shared store once per binary so the first timed run isn't
# paying for .drv writes / input fetches the others don't.
run_once "$BASE/bin/nix" >/dev/null
run_once "$NEW/bin/nix" >/dev/null

base_times=()
new_times=()
# Interleave base/new to cancel any drift in machine load over the run.
for ((i = 0; i < RUNS; i++)); do
    base_times+=("$(run_once "$BASE/bin/nix")")
    new_times+=("$(run_once "$NEW/bin/nix")")
done

base_med="$(printf '%s\n' "${base_times[@]}" | median)"
new_med="$(printf '%s\n' "${new_times[@]}" | median)"

printf 'regression gate: base=%s new=%s threshold=%s%% runs=%s\n' \
    "$BASE" "$NEW" "$THRESHOLD" "$RUNS"
printf '  base median: %.3fs\n' "$(awk -v n="$base_med" 'BEGIN { printf "%.3f", n / 1e9 }')"
printf '  new  median: %.3fs\n' "$(awk -v n="$new_med" 'BEGIN { printf "%.3f", n / 1e9 }')"
printf '  relative:    %.3fx\n' "$(awk -v b="$base_med" -v n="$new_med" 'BEGIN { printf "%.3f", n / b }')"

awk -v b="$base_med" -v n="$new_med" -v t="$THRESHOLD" 'BEGIN {
    if (n > b * (1 + t / 100.0)) {
        printf "  FAIL: %.1f%% slower than base (limit %.1f%%)\n", (n / b - 1) * 100, t;
        exit 1;
    }
    printf "  OK: within %.1f%% regression limit\n", t;
    exit 0;
}'
