#!/usr/bin/env bash
# Apply this project's local fixes to the xboxrecomp submodule.
#
# Usage: scripts/apply-patches.sh
#
# Each patches/xboxrecomp/*.patch is a general toolkit fix found while
# bringing up THUG (see docs/NOTES.md, "Toolkit patches"). They are meant to
# go upstream; until they land, a fresh clone needs them applied. The script
# is idempotent: a patch already in the tree is skipped.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLKIT="$ROOT/third_party/xboxrecomp"

shopt -s nullglob
for p in "$ROOT"/patches/xboxrecomp/*.patch; do
  name="$(basename "$p")"
  if git -C "$TOOLKIT" apply --check -R "$p" 2>/dev/null; then
    echo "==> $name: already applied"
  elif git -C "$TOOLKIT" apply --check "$p" 2>/dev/null; then
    git -C "$TOOLKIT" apply "$p"
    echo "==> $name: applied"
  else
    echo "error: $name neither applies nor is already applied." >&2
    echo "       The submodule may have moved; rebase the patch." >&2
    exit 1
  fi
done
