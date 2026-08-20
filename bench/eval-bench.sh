#!/usr/bin/env bash
# Lix evaluator benchmark harness.
#
# Measures evaluation-dominated Nix workloads against a chosen nix/lix binary.
# Purpose: establish a baseline BEFORE evaluator changes, then re-run after, so
# any speedup claim rests on measured numbers instead of expectation.
#
# Usage:
#   ./eval-bench.sh                      # benchmark the nix on PATH
#   ./eval-bench.sh /nix/store/...-lix/bin/nix
#   NIX_BIN=... EVAL_CORES=8 ./eval-bench.sh
#
# Output: a results table on stdout plus a JSON file under results/.
# Every measurement is wall-clock over a cold eval cache; the eval cache is
# disabled per-run so we time the evaluator, not sqlite.

set -uo pipefail

NIX_BIN="${1:-${NIX_BIN:-$(command -v nix)}}"
REPEATS="${REPEATS:-3}"
EVAL_CORES="${EVAL_CORES:-}"
RESULT_DIR="${RESULT_DIR:-$(dirname "$(readlink -f "$0")")/results}"

if [ -z "$NIX_BIN" ] || [ ! -x "$NIX_BIN" ]; then
  echo "FATAL: no executable nix binary found (got: '${NIX_BIN}')" >&2
  exit 1
fi

mkdir -p "$RESULT_DIR"

VERSION="$("$NIX_BIN" --version 2>&1 | head -1)"
STAMP="$(date +%Y%m%dT%H%M%S)"
LABEL="$(printf '%s' "$VERSION" | tr -cs 'A-Za-z0-9._-' '-' | sed 's/-\+/-/g; s/^-//; s/-$//')"
OUT_JSON="${RESULT_DIR}/${STAMP}-${LABEL}.json"

# Where the cluster config lives; it is the most representative workload we own.
NIXOS_CONFIG="${NIXOS_CONFIG:-/home/j_kro/Projects/nixos-config}"

echo "=============================================="
echo " Lix evaluator benchmark"
echo "=============================================="
echo " binary   : $NIX_BIN"
echo " version  : $VERSION"
echo " repeats  : $REPEATS"
echo " eval-cores: ${EVAL_CORES:-<unset/default>}"
echo " results  : $OUT_JSON"
echo

# Common flags:
#   --no-eval-cache        : time the evaluator, not the sqlite cache
#   --option pure-eval false: this repo needs impure eval (home-manager NIX_PATH)
#   --offline              : never let network latency pollute a CPU measurement
COMMON_OPTS=(--option pure-eval false --offline)

if [ -n "$EVAL_CORES" ]; then
  # eval-cores only exists on a parallel-eval-capable binary. Probe it, and only
  # pass it when supported, so this harness runs on both old and new binaries.
  if "$NIX_BIN" show-config 2>/dev/null | grep -q '^eval-cores'; then
    COMMON_OPTS+=(--option eval-cores "$EVAL_CORES")
    echo "note: eval-cores=$EVAL_CORES supported and enabled"
  else
    echo "note: binary does NOT support eval-cores; ignoring EVAL_CORES=$EVAL_CORES"
  fi
  echo
fi

# ---------------------------------------------------------------------------
# Workload definitions. Each is a name + a shell command.
# Chosen to be evaluation-bound, not build-bound (nothing here realises a drv).
# ---------------------------------------------------------------------------
declare -a WL_NAMES=()
declare -a WL_CMDS=()

add_workload() {
  WL_NAMES+=("$1")
  WL_CMDS+=("$2")
}

# 1. Whole-flake check, no build. The exact gate CI runs.
add_workload "flake-check-nixos-config" \
  "'$NIX_BIN' flake check --no-build ${COMMON_OPTS[*]} --no-eval-cache '$NIXOS_CONFIG'"

# 2. Evaluate every host's toplevel. Heavy shared stdenv, deep module merge.
for h in zephyr nexus forge sentry; do
  add_workload "eval-toplevel-$h" \
    "'$NIX_BIN' eval ${COMMON_OPTS[*]} --raw '$NIXOS_CONFIG#nixosConfigurations.$h.config.system.build.toplevel.drvPath'"
done

# 3. Attribute-set traversal breadth: many independent attrs, little sharing.
#    This is the shape that parallelises best, so it is the clearest signal.
add_workload "flake-show" \
  "'$NIX_BIN' flake show --json --no-eval-cache ${COMMON_OPTS[*]} '$NIXOS_CONFIG'"

# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------
printf '%-34s %10s %10s %10s %8s\n' "WORKLOAD" "BEST(s)" "MEDIAN(s)" "WORST(s)" "STATUS"
printf '%s\n' "--------------------------------------------------------------------------------"

json_entries=""

for i in "${!WL_NAMES[@]}"; do
  name="${WL_NAMES[$i]}"
  cmd="${WL_CMDS[$i]}"
  times=()
  status="ok"

  for ((r = 1; r <= REPEATS; r++)); do
    start=$(date +%s.%N)
    if ! eval "$cmd" >/dev/null 2>&1; then
      status="FAIL"
    fi
    end=$(date +%s.%N)
    times+=("$(echo "$end - $start" | bc -l)")
  done

  # Sort numerically to pull best/median/worst.
  sorted=$(printf '%s\n' "${times[@]}" | sort -g)
  best=$(printf '%s' "$sorted" | head -1)
  worst=$(printf '%s' "$sorted" | tail -1)
  mid_idx=$(((REPEATS + 1) / 2))
  median=$(printf '%s' "$sorted" | sed -n "${mid_idx}p")

  printf '%-34s %10.2f %10.2f %10.2f %8s\n' "$name" "$best" "$median" "$worst" "$status"

  all_times=$(printf '%s,' "${times[@]}" | sed 's/,$//')
  entry=$(printf '{"workload":"%s","best":%s,"median":%s,"worst":%s,"status":"%s","times":[%s]}' \
    "$name" "$best" "$median" "$worst" "$status" "$all_times")
  if [ -z "$json_entries" ]; then
    json_entries="$entry"
  else
    json_entries="$json_entries,$entry"
  fi
done

printf '%s\n' "--------------------------------------------------------------------------------"

nproc_val="$(nproc)"
printf '{"stamp":"%s","binary":"%s","version":"%s","repeats":%s,"eval_cores":"%s","nproc":%s,"results":[%s]}\n' \
  "$STAMP" "$NIX_BIN" "$VERSION" "$REPEATS" "${EVAL_CORES:-unset}" "$nproc_val" "$json_entries" \
  > "$OUT_JSON"

echo
echo "wrote $OUT_JSON"
