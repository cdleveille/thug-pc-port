#!/usr/bin/env bash
# Play the built game.
#
# Usage: scripts/play.sh [options]
#   --fullscreen     start in borderless fullscreen (F11 or Alt+Enter toggles)
#   --4x3            4:3 instead of widescreen
#   --scale <x>      internal resolution as a multiple of 480 lines
#                    (default: your desktop's height, e.g. 2.25 for 1080p)
#   --no-vsync       don't wait for the display
#   --log <file>     where the game's log goes (default: stderr.txt)
#
# In game, F1 (or Back+Start on a pad) opens the settings overlay; what you
# change there is saved to ~/.config/thug-pc-port/settings.ini. The options
# above override the saved settings for this run, as does any THUG_* or
# RECOMP_* variable set in the environment (see README.md). The log is
# overwritten each run; the previous one is kept as stderr.prev.txt.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"   # the game reads game/ relative to here

LOG=stderr.txt
while [[ $# -gt 0 ]]; do
  case "$1" in
    --fullscreen) export THUG_FULLSCREEN=1 ;;
    --4x3)        export THUG_WIDESCREEN=0 ;;
    --scale)      export THUG_RES_SCALE="${2:?--scale needs a value}"; shift ;;
    --no-vsync)   export THUG_VSYNC=0 ;;
    --log)        LOG="${2:?--log needs a file}"; shift ;;
    -h|--help)    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "error: unknown option '$1' (try --help)" >&2; exit 1 ;;
  esac
  shift
done

if [[ ! -x build/bin/thug ]]; then
  echo "error: build/bin/thug not found. Run scripts/build.sh first." >&2
  exit 1
fi
if [[ ! -f game/default.xbe ]]; then
  echo "error: game/default.xbe not found. Run scripts/build.sh (it extracts rom/*.iso)." >&2
  exit 1
fi

[[ -f "$LOG" ]] && mv -f "$LOG" "${LOG%.txt}.prev.txt"
echo "Starting Tony Hawk's Underground (log: $LOG). F1: settings. F11 or Alt+Enter: fullscreen."
build/bin/thug 2> "$LOG"
rc=$?
if [[ $rc -ne 0 && $rc -ne 130 ]]; then
  echo "The game exited with code $rc. Last lines of $LOG:" >&2
  tail -n 15 "$LOG" >&2
fi
exit $rc
