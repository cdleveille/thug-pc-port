# The Linux release's build environment (scripts/release.sh builds and runs
# it). Ubuntu 22.04 sets the oldest glibc the release runs on, 2.35; clang
# comes from apt.llvm.org, the same major version the port is developed with.
#
# The -dev packages are SDL's back ends (X11, Wayland, sound, udev, D-Bus):
# headers only, since SDL loads each library at run time. Leaving one out
# leaves that back end out of the game, which CMakeLists.txt turns into an
# error for a release (THUG_REQUIRE_SDL_BACKENDS).
FROM ubuntu:22.04

ARG LLVM_VERSION=23
# The fingerprint of apt.llvm.org's signing key (Sylvestre Ledru - Debian
# LLVM packages).
ARG LLVM_SIGNER=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /tmp/llvm.asc \
 && test "$(gpg --show-keys --with-colons /tmp/llvm.asc | awk -F: '/^fpr/ { print $10; exit }')" = "$LLVM_SIGNER" \
 && install -d -m 0755 /etc/apt/keyrings \
 && gpg --dearmor -o /etc/apt/keyrings/apt.llvm.org.gpg /tmp/llvm.asc \
 && echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/jammy/ llvm-toolchain-jammy-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      clang-${LLVM_VERSION} g++ binutils cmake ninja-build python3 \
      libx11-dev libxext-dev libxcursor-dev libxfixes-dev libxi-dev libxrandr-dev libxss-dev \
      libgl-dev libegl-dev libgles-dev \
      libwayland-dev libxkbcommon-dev libdecor-0-dev \
      libdrm-dev libgbm-dev \
      libasound2-dev libpulse-dev libpipewire-0.3-dev libjack-jackd2-dev libsndio-dev \
      libsamplerate0-dev \
      libudev-dev libdbus-1-dev \
 && rm -rf /var/lib/apt/lists/* /tmp/llvm.asc /root/.gnupg

ENV CC=clang-${LLVM_VERSION} CXX=clang++-${LLVM_VERSION}
