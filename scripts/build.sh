#!/usr/bin/env bash
# Rebuild the game: everything from the disc image to build/bin/thug, doing
# only the steps that are out of date.
#
# Usage: scripts/build.sh [--regen] [--release]
#   --regen    lift default.xbe again from scratch (scripts/regen.sh)
#   --release  configure a Release build instead of RelWithDebInfo
#              (only takes effect when build/ is configured for the first time)
#
# Steps, each skipped when it has nothing to do:
#   1. toolkit patches (scripts/apply-patches.sh)
#   2. disc extraction, if game/default.xbe is missing (scripts/extract.sh)
#   3. lifting, if src/recomp/gen/ is empty, if config/seed_functions.json
#      changed (full run), or if src/recomp_manual.c changed (recomp stage
#      only: an override added or removed changes which functions are
#      generated, and a stale set fails to link)
#   4. CMake configure (first time) and build
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

REGEN=0
BUILD_TYPE=RelWithDebInfo
for arg in "$@"; do
  case "$arg" in
    --regen)   REGEN=1 ;;
    --release) BUILD_TYPE=Release ;;
    -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "error: unknown option '$arg' (try --help)" >&2; exit 1 ;;
  esac
done

step() { printf '\n==> %s\n' "$*"; }

# 1. Toolkit patches. A tree with local toolkit edits beyond the patch makes
#    the script report "neither applies nor is already applied"; that is a
#    developer's working tree, not a fresh checkout, so carry on.
if [[ ! -f third_party/xboxrecomp/CMakeLists.txt || ! -f third_party/imgui/imgui.cpp ]]; then
  step "fetching submodules (xboxrecomp, Dear ImGui)"
  git submodule update --init --recursive
fi
step "toolkit patches"
if ! scripts/apply-patches.sh; then
  echo "warning: the toolkit has changes beyond patches/xboxrecomp; building it as it is" >&2
fi

# 2. The game files.
if [[ ! -f game/default.xbe ]]; then
  step "extracting the disc image"
  scripts/extract.sh
fi

# 3. The lifted C. The stamp records when it was last generated: the
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

# 4. Compile.
if [[ ! -f build/build.ninja ]]; then
  step "configuring build/ ($BUILD_TYPE)"
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi
step "building"
cmake --build build

step "done: build/bin/thug (scripts/play.sh to play)"
