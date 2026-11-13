#!/usr/bin/env bash
# Build the Linux release, for glibc 2.35 and newer (Ubuntu 22.04, Debian 12,
# Fedora 36 and later), into build/release/:
#   thug-pc-port-<version>-linux-x86_64/          the game, README.txt,
#                                                  licenses/, and game/ for
#                                                  the player's disc image
#   thug-pc-port-<version>-linux-x86_64.tar.gz    the same, to upload
#   thug-pc-port-<version>-linux-x86_64.tar.gz.sha256
#   thug-pc-port-<version>-linux-x86_64.debug     debug info (not shipped)
#
# Usage: scripts/release.sh
#
# Needs Docker. The lifted C is brought up to date on this machine first
# (scripts/build.sh --lift-only: this machine's Python and capstone, the
# lifter the port is tested with). The compile runs in an Ubuntu 22.04
# container (scripts/release/Dockerfile and package.sh) that mounts the
# repository; nothing is copied into the image, and nothing from game/ goes
# into the release. Its build tree is build/release-build/.
#
# Environment:
#   JOBS     compile jobs (default: as many as the container's memory allows,
#            about 700 MB each; Docker Desktop's VM has 8 GB by default)
#   VERSION  the version in the release's name (default: git describe)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

IMAGE=thug-pc-port-release:ubuntu22.04-llvm23

step() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

case "${1:-}" in
  "") ;;
  -h|--help) sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *) die "unknown option '$1' (try --help)" ;;
esac

command -v docker >/dev/null || die "Docker is not installed."
docker info >/dev/null 2>&1 || die "Docker is not running (start Docker Desktop or the docker service)."

VERSION="${VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo unknown)}"

step "lifted C"
scripts/build.sh --lift-only

step "container image $IMAGE (built once, then cached)"
docker build -t "$IMAGE" - < scripts/release/Dockerfile

if [[ -z ${JOBS:-} ]]; then
  read -r cpus mem < <(docker info --format '{{.NCPU}} {{.MemTotal}}')
  JOBS=$(( mem / (700 * 1024 * 1024) ))
  (( JOBS > cpus )) && JOBS=$cpus
  (( JOBS < 1 )) && JOBS=1
fi

run=(docker run --rm --init
     -v "$ROOT:$ROOT" -w "$ROOT"
     -e HOME=/tmp -e THUG_VERSION="$VERSION" -e JOBS="$JOBS")
# A system Docker daemon runs the container as root unless told otherwise,
# which would leave root-owned files in build/. Rootless Docker maps its root
# to you already, and --user there would map to someone else.
if ! docker info --format '{{.SecurityOptions}}' | grep -q rootless; then
  run+=(--user "$(id -u):$(id -g)")
fi
# A terminal, so Ctrl+C reaches the build inside.
[[ -t 0 && -t 1 ]] && run+=(-it)

step "release $VERSION, compiled in the container"
"${run[@]}" "$IMAGE" scripts/release/package.sh

echo
echo "Release: build/release/thug-pc-port-$VERSION-linux-x86_64.tar.gz"
