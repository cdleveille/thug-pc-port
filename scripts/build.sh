#!/usr/bin/env bash
# Rebuild the game: everything from the disc image to build/bin/thug, doing
# only the steps that are out of date.
#
# Usage: scripts/build.sh [--regen] [--release] [--lift-only]
#   --regen      lift default.xbe again from scratch (scripts/regen.sh)
#   --release    configure a Release build instead of RelWithDebInfo
#                (only takes effect when build/ is configured for the first time)
#   --lift-only  stop after step 2 (scripts/release.sh compiles the lifted C
#                in a container)
#
# Put your disc image (.iso) in game/ first. Steps, each skipped when it has
# nothing to do:
#   1. CMake configure (first time) and build/bin/thug-extract, which unpacks
#      the image into game/ if default.xbe is missing or not the supported
#      build (the game itself does the same at start-up)
#   2. lifting, if src/recomp/gen/ is empty, if config/seed_functions.json
#      changed (full run), or if src/recomp_manual.c changed (recomp stage
#      only: an override added or removed changes which functions are
#      generated, and a stale set fails to link)
#   3. building the game
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

REGEN=0
BUILD_TYPE=RelWithDebInfo
LIFT_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --regen)     REGEN=1 ;;
    --release)   BUILD_TYPE=Release ;;
    --lift-only) LIFT_ONLY=1 ;;
    -h|--help)   sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "error: unknown option '$arg' (try --help)" >&2; exit 1 ;;
  esac
done

step() { printf '\n==> %s\n' "$*"; }

# 1. The game files, which lifting reads. thug-extract is the game's own
#    start-up check, built on its own because the game cannot be built yet.
if [[ ! -f build/build.ninja ]]; then
  step "configuring build/ ($BUILD_TYPE)"
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi
cmake --build build --target thug-extract > /dev/null
step "game files"
build/bin/thug-extract "$ROOT/game"

# 2. The lifted C. The stamp records when it was last generated: the
#    generator leaves files whose content did not change untouched, so their
#    own times cannot say whether a run is still needed.
GEN_DIR=src/recomp/gen
STAMP=$GEN_DIR/.regen-stamp
shopt -s nullglob
gen=($GEN_DIR/*.c)
shopt -u nullglob
MARK=$STAMP
[[ -f $STAMP ]] || MARK=$GEN_DIR/recomp_dispatch.c
if [[ $REGEN -eq 1 || ${#gen[@]} -eq 0 || ! -f $MARK ]]; then
  step "lifting default.xbe (a few minutes)"
  scripts/regen.sh
  touch "$STAMP"
elif [[ config/seed_functions.json -nt $MARK ]]; then
  step "seed functions changed: lifting again (a few minutes)"
  scripts/regen.sh
  touch "$STAMP"
elif [[ src/recomp_manual.c -nt $MARK ]]; then
  step "manual overrides changed: regenerating the C"
  scripts/regen.sh --from recomp
  touch "$STAMP"
fi
if [[ $LIFT_ONLY -eq 1 ]]; then
  step "lifted C is up to date"
  exit 0
fi

# 3. Compile. (The configure step re-globs src/recomp/gen/ by itself.)
step "building"
cmake --build build

step "done: run build/bin/thug"
