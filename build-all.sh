#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build everything load.sh needs, from an empty work volume, in order:
#   1. the native Buildroot (toolchain, uClibc-ng FDPIC, busybox): its
#      toolchain builds the kernel, its tree seeds the pins build, and it
#      downloads the busybox source busybox-atrans is built from
#   2. the ATRANS toolchain and packages, claude-guest-mcp
#      -> out/atrans-root, out/atrans-sysroot.tar.gz
#   3. tcc for the board against that sysroot, the lean tree -> out/tcc-root
#   4. the pins userland (libpio, MicroPython, ...; PIO tests built by the
#      ATRANS gcc and by tcc) -> out/pins-root
#   5. the root filesystem again, with the three trees, and the kernel
#      -> out/rootfs.squashfs, out/xip.romfs, out/xipImage, out/rp2350.dtb
#   6. the boot stub -> boot/target/thumbv8m.main-none-eabihf/release/nativeboot
# Each step is incremental: running it again rebuilds what changed.
# Then: ./load.sh (it never formats /home or touches the board config).
set -euo pipefail
cd "$(dirname "$0")"
step() { printf '\n==== build-all: %s\n' "$*"; }

step "1/6 native rootfs (Buildroot)"
./run-build.sh rootfs
step "2/6 ATRANS toolchain, programs, tree"
atrans/run-build.sh all
step "3/6 tcc"
scratch_patch=tcc/patches/0019-armv7m-spill-link-text-to-scratch.patch
if [ ! -d tcc/tinycc ]; then
  tcc/apply-patches.sh
elif ! patch -Rs -f --dry-run -d tcc/tinycc -p1 < "$scratch_patch" >/dev/null 2>&1; then
  if ! patch -s -f --dry-run -d tcc/tinycc -p1 < "$scratch_patch" >/dev/null 2>&1; then
    echo "The TinyCC tree differs from the bundled patches. Move tcc/tinycc aside and rebuild." >&2
    exit 1
  fi
  patch -f -d tcc/tinycc -p1 < "$scratch_patch"
fi
tcc/run-host.sh sh /native/tcc/import-sysroot.sh /native/out/atrans-sysroot.tar.gz
tcc/run-host.sh sh /native/tcc/host-build.sh
BOARD_LIBC=/usr/lib/atrans tcc/run-host.sh sh /native/tcc/native-build.sh /native/out/tcc-root
step "4/6 pins"
pins/run-build.sh root
step "5/6 rootfs with the trees, kernel"
./run-build.sh all
step "6/6 boot stub"
(cd boot && cargo build --release --locked)
ls -l out/xipImage out/rp2350.dtb out/rootfs.squashfs out/xip.romfs \
  boot/target/thumbv8m.main-none-eabihf/release/nativeboot
