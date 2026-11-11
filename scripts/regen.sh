#!/usr/bin/env bash
# Regenerate the lifted C from game/default.xbe.
#
# Usage: scripts/regen.sh [--from STAGE]
#   STAGE is one of: parse, disasm, func_id, abi, recomp (default: parse)
#
# Runs the xboxrecomp pipeline from inside the toolkit (that is where tools/
# lives). Intermediate JSON goes to the toolkit's own gitignored
# tools/*/output/ directories; the generated C goes to src/recomp/gen/.
#
# Never edit src/recomp/gen/ by hand: this script overwrites it. Fixes belong
# in config/ (seeds), src/recomp_manual.c (overrides), or the toolkit.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLKIT="${XBOXRECOMP_DIR:-$ROOT/third_party/xboxrecomp}"
XBE="$ROOT/game/default.xbe"
ANALYSIS="$ROOT/game/default_analysis.json"
GEN_DIR="$ROOT/src/recomp/gen"
SEEDS="$ROOT/config/seed_functions.json"
MANUAL="$ROOT/src/recomp_manual.c"

FROM="parse"
if [[ "${1:-}" == "--from" ]]; then
  FROM="${2:?--from needs a stage}"
fi

stages=(parse disasm func_id abi recomp)
run=0
should_run() {
  [[ $run -eq 1 || "$1" == "$FROM" ]] && run=1
  [[ $run -eq 1 ]]
}

if [[ ! -f "$XBE" ]]; then
  echo "error: $XBE not found. Put your disc image in game/ and run scripts/build.sh." >&2
  exit 1
fi
if ! python3 -c 'import capstone' 2>/dev/null; then
  echo "error: Python module 'capstone' is missing (Arch: pacman -S python-capstone)." >&2
  exit 1
fi

cd "$TOOLKIT"

if should_run parse; then
  echo "==> [1/5] xbe_parser"
  python3 -m tools.xbe_parser "$XBE" --json "$ANALYSIS" > /dev/null
fi

if should_run disasm; then
  # All code sections the loader accepts (every executable section that is not
  # a conventional data section). Not --text-only: THUG statically links D3D,
  # DSOUND, WMADEC, Bink, XNET and XPP, and the game calls straight into them.
  seed_args=()
  [[ -f "$SEEDS" ]] && seed_args=(--seed-functions "$SEEDS")
  echo "==> [2/5] disasm"
  python3 -m tools.disasm "$XBE" -v --force "${seed_args[@]}"
fi

if should_run func_id; then
  echo "==> [3/5] func_id"
  python3 -m tools.func_id "$XBE" -v
fi

if should_run abi; then
  # Never skip: without abi_functions.json every signature silently falls
  # back to cdecl / 0 params.
  echo "==> [4/5] abi_analysis"
  python3 -m tools.abi_analysis "$XBE" -v
fi

if should_run recomp; then
  # --split 250: larger chunks have OOM-killed 15 GB machines at compile time.
  echo "==> [5/5] recomp -> $GEN_DIR"
  mkdir -p "$GEN_DIR"
  python3 -m tools.recomp "$XBE" --all --split 250 \
    --gen-dir "$GEN_DIR" --exclude-manual "$MANUAL"
fi

if [[ $run -eq 0 ]]; then
  echo "error: unknown stage '$FROM' (one of: ${stages[*]})" >&2
  exit 1
fi

echo "==> done. Build with: cmake -S . -B build -G Ninja && cmake --build build"
