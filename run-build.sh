#!/usr/bin/env bash
# Host wrapper: build native Arm Linux for the RP2350 in Docker.
# Usage: [SRAM_DATA=1] [NATIVE_TAG=name] ./run-build.sh [all|rootfs|kernel]
#   rootfs: Buildroot (toolchain, uClibc-ng FDPIC, busybox) -> out/rootfs.squashfs, out/xip.romfs
#   kernel: Linux (nommu, XIP from flash) -> out/xipImage, out/rp2350.dtb
# Runs in the rp2350-native-build image (docker/Dockerfile) with the
# Buildroot checkout in src/buildroot (cloned at BR_TAG below when missing);
# build trees go in the work volume (WORK_VOLUME, default
# rp2350-native-work) under /work/arm-*.
# SRAM_DATA=1 links the kernel's .data/.bss into SRAM (linux-sram-data.fragment).
# NATIVE_TAG=name builds in trees of its own (see build.sh), for parallel work.
set -euo pipefail
cd "$(dirname "$0")"
. tools/preflight.sh
# Buildroot is not in the repository: the release the configs are for
BR_TAG=2026.08.1
BR_REV=7fdeadb3b9f12f78e5fccca914d7fef47090ab9a
stage=${1:-all}
case "$stage" in all|rootfs|kernel) ;; *) echo "Usage: $0 [all|rootfs|kernel]" >&2; exit 2;; esac
for tool in git docker; do need_tool "$tool" 'See README.md.'; done
docker info >/dev/null || { echo 'Start Docker and retry.' >&2; exit 1; }
buildroot_src "$PWD"
mkdir -p out
docker build -q -t $BUILD_IMAGE docker >/dev/null
docker volume create $WORK_VOLUME >/dev/null
docker run --rm -v "$PWD/src:/src" -v "$PWD/out:/out" -v "$PWD:/native:ro" \
  -v $WORK_VOLUME:/work --user 0 $BUILD_IMAGE bash -c '
    set -e
    trap "chown -R $1 /out" EXIT
    chown builder /work; chown -R builder /out
    su builder -c "SRAM_DATA=$3 NATIVE_TAG=$4 PINS_TESTFS=$5 /native/build.sh $2"
  ' _ "$(id -u):$(id -g)" "$stage" "${SRAM_DATA:-}" "${NATIVE_TAG:-}" "${PINS_TESTFS:-}" 2>&1 | tee "out/build-$stage.log"
