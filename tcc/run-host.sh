#!/usr/bin/env bash
# run-host.sh CMD...: run CMD in the rp2350-native-tcc image (Dockerfile in
# docker/: gcc, arm-none-eabi-gcc, qemu-arm) with /native = the repository,
# /tcc-src = the patched tinycc tree (tinycc/, see apply-patches.sh), and
# the work volume (WORK_VOLUME, default rp2350-native-work) at /work. Example:
#   tcc/run-host.sh sh /native/tcc/host-build.sh
# SYSROOT, TARGET_CC, XB, NB, BOARD_LIBC, LAYOUT and ROOT are passed through (see env.sh
# and native-build.sh).
set -euo pipefail
cd "$(dirname "$0")"
. ../tools/preflight.sh
docker build -q -t rp2350-native-tcc docker >/dev/null
docker volume create $WORK_VOLUME >/dev/null
exec docker run --rm -i -e SYSROOT -e TARGET_CC -e XB -e NB -e BOARD_LIBC -e LAYOUT -e ROOT -v "$PWD/..:/native" -v "$PWD/tinycc:/tcc-src" \
	-v $WORK_VOLUME:/work rp2350-native-tcc "$@"
