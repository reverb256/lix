#!/bin/bash
# Test additional PGO training workloads for coverage improvement.
# Run from INSIDE nix develop: nix develop --command bash bench/test-pgo-workloads.sh
# Uses temp files to avoid bash quoting nightmares with Nix expressions.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILDDIR=build-inst
NIX_BIN="./$BUILDDIR/lix/nix/nix"
export LD_LIBRARY_PATH="./$BUILDDIR/libexpr:./$BUILDDIR/libutil:./$BUILDDIR/libfetchers"
export NIX_REMOTE=local
export NIX_STATE_DIR=$(mktemp -d -t pgo-state-XXXXX)
mkdir -p "$NIX_STATE_DIR"

PROFDATA=/tmp/lixpgo/merged.profdata
TMPDIR=$(mktemp -d -t pgo-wl-XXXXX)

# Verify
test -x "./$BUILDDIR/lix/nix/nix" || { echo "ERROR: instrumented binary not found"; exit 1; }
test -f "$PROFDATA" || { echo "ERROR: baseline profile not found"; exit 1; }

# Get baseline stats
base_funcs=$(llvm-profdata show "$PROFDATA" 2>/dev/null | grep '^Total functions' | awk '{print $NF}')
base_count=$(llvm-profdata show "$PROFDATA" 2>/dev/null | grep '^Total count' | awk '{print $NF}')

echo "Instrumented: ./$BUILDDIR/lix/nix/nix"
echo "Baseline: functions=$base_funcs  count=$base_count"
echo ""
printf "%-22s %6s  %8s %7s  %14s\n" "WORKLOAD" "TIME" "FUNCS" "DELTA_F" "DELTA_COUNT"
printf "%-22s %6s  %8s %7s  %14s\n" "-------" "----" "-----" "-------" "-----------"

run_workload() {
    local name="$1"
    local exprfile="$2"

    local profraw="$TMPDIR/$name-%p.profraw"

    local t0=$(date +%s%N)
    set +e
    LLVM_PROFILE_FILE="$profraw" timeout 60 "$NIX_BIN" eval -f "$exprfile" > /dev/null 2>&1
    local rc=$?
    set -e
    local t1=$(date +%s%N)
    local ms=$(( (t1 - t0) / 1000000 ))

    if [ $rc -ne 0 ]; then
        printf "%-22s %5dms  %s\n" "$name" "$ms" "FAIL"
        return
    fi

    local raw=$(ls "$TMPDIR"/*.profraw 2>/dev/null | head -1)
    if [ -z "$raw" ] || [ ! -s "$raw" ]; then
        printf "%-22s %5dms  %s\n" "$name" "$ms" "NO DATA"
        return
    fi

    # Merge with baseline
    local merged=$(mktemp -t pgo-merged-XXXXX)
    llvm-profdata merge -output="$merged" "$PROFDATA" "$raw" 2>/dev/null

    local new_funcs=$(llvm-profdata show "$merged" 2>/dev/null | grep '^Total functions' | awk '{print $NF}')
    local new_count=$(llvm-profdata show "$merged" 2>/dev/null | grep '^Total count' | awk '{print $NF}')
    local fdelta=$((new_funcs - base_funcs))
    local cdelta=$((new_count - base_count))

    printf "%-22s %5dms  %8s %+7d  %+14d\n" "$name" "$ms" "$new_funcs" "$fdelta" "$cdelta"

    rm -f "$merged"
}

# Write each workload to a temp file and run it

# 1: String operations
cat > "$TMPDIR/strings.nix" << 'NIXEOF'
let
  strs = builtins.genList (i: builtins.toString i) 5000;
  joined = builtins.concatStringsSep "-" strs;
  replaced = builtins.replaceStrings ["3"] ["III"] joined;
  len = builtins.stringLength replaced;
  sub = builtins.substring 0 1000 replaced;
  parts = builtins.split "-" sub;
in builtins.length parts
NIXEOF
run_workload "strings" "$TMPDIR/strings.nix"

# 2: Arithmetic
cat > "$TMPDIR/arithmetic.nix" << 'NIXEOF'
let
  range = builtins.genList (i: i) 3000;
  sum = builtins.foldl' builtins.add 0 range;
  product = builtins.foldl' builtins.mul 1 (builtins.genList (i: i + 1) 10);
  bitwise = builtins.foldl' builtins.bitOr 0 (builtins.genList (i: builtins.bitAnd i 255) 1000);
in sum + product + bitwise
NIXEOF
run_workload "arithmetic" "$TMPDIR/arithmetic.nix"

# 3: JSON
cat > "$TMPDIR/json.nix" << 'NIXEOF'
let
  obj = builtins.genList (i: {
    name = "pkg-${builtins.toString i}";
    version = "${builtins.toString i}.0.0";
    deps = builtins.genList (j: "dep-${builtins.toString j}") (i / 10 + 1);
  }) 500;
  jsonStr = builtins.toJSON obj;
  parsed = builtins.fromJSON jsonStr;
in builtins.length parsed
NIXEOF
run_workload "json" "$TMPDIR/json.nix"

# 4: Pattern matching
cat > "$TMPDIR/patterns.nix" << 'NIXEOF'
let
  testMatch = s:
    let m = builtins.match "([a-z]+)-([0-9]+)" s;
    in if m == null then "" else builtins.elemAt m 0;
  inputs = builtins.genList (i: "foo-${builtins.toString (i + 1)}") 2000;
  results = builtins.map testMatch inputs;
  nonEmpty = builtins.filter (x: x != "") results;
in builtins.length nonEmpty
NIXEOF
run_workload "patterns" "$TMPDIR/patterns.nix"

# 5: Lambda-heavy (compose, mapAttrs, filterAttrs)
cat > "$TMPDIR/lambdas.nix" << 'NIXEOF'
let
  compose = f: g: x: f (g x);
  add1 = x: x + 1;
  double = x: x * 2;
  pipeline = builtins.foldl' (acc: f: compose f acc) (x: x)
    (builtins.genList (_: add1) 500);
  result = pipeline 0;
  attrs = builtins.genList (i: { name = "a${builtins.toString i}"; val = i; }) 3000;
  attrset = builtins.listToAttrs attrs;
  mapped = builtins.mapAttrs (n: v: v * 2) attrset;
  filtered = builtins.filterAttrs (n: v: v > 1000 && v < 2000) mapped;
in builtins.length (builtins.attrNames filtered)
NIXEOF
run_workload "lambdas" "$TMPDIR/lambdas.nix"

# 6: tryEval error handling
cat > "$TMPDIR/tryeval.nix" << 'NIXEOF'
let
  safeDiv = x: y:
    if y == 0 then 0
    else let r = builtins.tryEval (x / y);
         in if r.success then r.value else 0;
  range = builtins.genList (i: i) 1000;
  results = builtins.map (i: safeDiv 1000000 i) range;
  sum = builtins.foldl' builtins.add 0 results;
in sum
NIXEOF
run_workload "tryeval" "$TMPDIR/tryeval.nix"

# 7: DeepSeq (nested attrsets, deep forcing)
cat > "$TMPDIR/deepseq.nix" << 'NIXEOF'
let
  deepAttrset = builtins.genList (i: {
    id = i;
    nested = {
      a = builtins.genList (j: { x = j; y = j * j; }) 20;
      b = builtins.genList (j: "${builtins.toString j}-${builtins.toString (j*j)}") 20;
    };
  }) 200;
  _ = builtins.deepSeq deepAttrset 0;
  forced = map (x: x.nested.a) deepAttrset;
  flat = builtins.concatLists forced;
in builtins.length flat
NIXEOF
run_workload "deepseq" "$TMPDIR/deepseq.nix"

# 8: Sort + groupBy
cat > "$TMPDIR/sort-group.nix" << 'NIXEOF'
let
  items = builtins.genList (i: {
    key = builtins.toString (5000 - i);
    val = i;
  }) 5000;
  sorted = builtins.sort (a: b: a.key < b.key) items;
  groups = builtins.groupBy (x: builtins.toString (x.val / 100)) items;
  groupNames = builtins.attrNames groups;
in builtins.length sorted + builtins.length groupNames
NIXEOF
run_workload "sort-group" "$TMPDIR/sort-group.nix"

# 9: String interpolation heavy
cat > "$TMPDIR/interpolation.nix" << 'NIXEOF'
let
  parts = builtins.genList (i: "${builtins.toString i} items and stuff") 5000;
  result = builtins.concatStringsSep ", " parts;
  len = builtins.stringLength result;
in len
NIXEOF
run_workload "interpolation" "$TMPDIR/interpolation.nix"

# 10: Assert + recursive update (//)
cat > "$TMPDIR/assert-with.nix" << 'NIXEOF'
let
  f = x: assert x > 0; { a = x; } // { b = x * 2; };
  range = builtins.genList (i: i + 1) 1000;
  results = builtins.map f range;
  sum = builtins.foldl' (acc: x: acc + x.a + x.b) 0 results;
in sum
NIXEOF
run_workload "assert-with" "$TMPDIR/assert-with.nix"

# 11: genericClosure
cat > "$TMPDIR/generic-closure.nix" << 'NIXEOF'
builtins.genericClosure {
  startSet = builtins.genList (i: { key = i; }) 100;
  operator = item: builtins.genList (i: { key = item.key + i + 1; }) 3;
}
NIXEOF
run_workload "generic-closure" "$TMPDIR/generic-closure.nix"

# 12: Heavy conditionals (if/then/else chains — ExprIf hot path)
cat > "$TMPDIR/conditionals.nix" << 'NIXEOF'
let
  classify = x:
    if x < 0 then "negative"
    else if x == 0 then "zero"
    else if x < 100 then "small"
    else if x < 1000 then "medium"
    else if x < 10000 then "large"
    else "huge";
  range = builtins.genList (i: i - 5000) 10000;
  classified = builtins.map classify range;
  counts = builtins.foldl' (acc: s: acc // { "${s}" = (acc."${s}" or 0) + 1; }) {} classified;
in builtins.length (builtins.attrNames counts)
NIXEOF
run_workload "conditionals" "$TMPDIR/conditionals.nix"

# 13: Has-attribute checks (ExprOpHasAttr hot path)
cat > "$TMPDIR/hasattr.nix" << 'NIXEOF'
let
  makeSet = i: {
    name = "item-${builtins.toString i}";
    value = i;
    extra = i * 2;
  };
  items = builtins.genList makeSet 5000;
  sets = builtins.map (x: { inherit (x) name value; }) items;
  # Check has-attr in a tight loop
  checkAttrs = s: (s ? name) && (s ? value) && (! (s ? nonexistent));
  results = builtins.map checkAttrs sets;
  valid = builtins.filter (x: x) results;
in builtins.length valid
NIXEOF
run_workload "hasattr" "$TMPDIR/hasattr.nix"

# 14: Select with default (ExprSelect hot path)
cat > "$TMPDIR/select.nix" << 'NIXEOF'
let
  makeSet = i: {
    a = builtins.toString i;
    b = i * 2;
    c = { d = i + 1; e = i - 1; };
  };
  sets = builtins.genList makeSet 5000;
  # Select with defaults in a tight loop
  extract = s: (s.a or "default") + (builtins.toString (s.b or 0));
  cExtract = s: builtins.toString ((s.c.d or 0) + (s.c.e or 0));
  results = builtins.map (s: extract s + cExtract s) sets;
in builtins.length results
NIXEOF
run_workload "select" "$TMPDIR/select.nix"

# 15: List concat + elem (ExprOpConcatLists + list ops)
cat > "$TMPDIR/lists.nix" << 'NIXEOF'
let
  lists = builtins.genList (i:
    builtins.genList (j: i * 1000 + j) 10
  ) 500;
  flat = builtins.concatLists lists;
  a = builtins.genList (i: i) 1000;
  b = builtins.genList (i: i + 500) 1000;
  merged = a ++ b;
  unique = builtins.filter (x: builtins.elem x (builtins.genList (i: i * 2) 500)) merged;
in builtins.length flat + builtins.length merged + builtins.length unique
NIXEOF
run_workload "lists" "$TMPDIR/lists.nix"

echo ""
rm -rf "$TMPDIR" "$NIX_STATE_DIR"
