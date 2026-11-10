#!/usr/bin/env bash
# Unpack your own Tony Hawk's Underground (Xbox, USA) disc image into game/.
#
# Usage: scripts/extract.sh [path/to/image.iso]
#        (default: the single .iso in rom/)
#
# Uses the toolkit's own XDVDFS reader, which finds the game partition in a
# full Redump image (video partition first, game partition at 0x18300000) as
# well as in a trimmed XISO.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLKIT="${XBOXRECOMP_DIR:-$ROOT/third_party/xboxrecomp}"
OUT="$ROOT/game"

# SHA-1 of default.xbe from the NTSC-U retail disc this port targets.
EXPECTED_XBE_SHA1="2951de954c96a8b962ba1cde34548fa82ae668d7"

ISO="${1:-}"
if [[ -z "$ISO" ]]; then
  shopt -s nullglob
  isos=("$ROOT"/rom/*.iso)
  if [[ ${#isos[@]} -ne 1 ]]; then
    echo "error: pass the disc image path, or put exactly one .iso in rom/" >&2
    exit 1
  fi
  ISO="${isos[0]}"
fi
ISO="$(cd "$(dirname "$ISO")" && pwd)/$(basename "$ISO")"

echo "==> extracting $ISO -> $OUT"
cd "$TOOLKIT"
python3 -m tools.xiso unpack "$ISO" -o "$OUT"

actual="$(sha1sum "$OUT/default.xbe" | cut -d' ' -f1)"
if [[ "$actual" != "$EXPECTED_XBE_SHA1" ]]; then
  echo "warning: default.xbe SHA-1 is $actual" >&2
  echo "         expected $EXPECTED_XBE_SHA1 (USA retail)." >&2
  echo "         Addresses in this project are for that build and will not match." >&2
else
  echo "==> default.xbe matches the supported USA retail build"
fi
