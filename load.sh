#!/usr/bin/env bash
# Load native Linux onto the board over USB BOOTSEL (picotool):
#   the boot stub (boot/) at 0x10000000,
#   out/rootfs.squashfs at 0x10400000 (the root filesystem, at most
#   0x980000 bytes, up to the kernel), only when it differs from what the
#   board holds (picotool verify),
#   out/xipImage at 0x10D80000 (CONFIG_XIP_PHYS_ADDR; at most 0x270000
#   bytes, up to the scratch region at 0x10FF0000),
#   out/rp2350.dtb at 0x103F8000,
#   out/xip.romfs at 0x10200000 (the xip romfs: text that runs in place
#   from flash, mkxiprom.py; at most 0x1F8000 bytes, up to the DTB), only
#   when it differs from what the board holds.
# usage: load.sh [--stub-only]
# Never written: the Wi-Fi settings sector (0x103FF000) and /home
# (0x10100000-0x101FFFFF, the user's jffs2).
# 0x10200000-0x103F7FFF holds the xip romfs.
set -euo pipefail
cd "$(dirname "$0")"
P=${PROBE_RS:-$HOME/git/toolchains/probe-rs-master/bin/probe-rs}
STUB_DIR=boot
STUB=$STUB_DIR/target/thumbv8m.main-none-eabihf/release/nativeboot
KERNEL_ADDR=0x10D80000
KERNEL_MAX=0x270000
DTB_ADDR=0x103F8000
ROOTFS_ADDR=0x10400000
ROOTFS_MAX=0x980000
XIP_ADDR=0x10200000
XIP_MAX=0x1F8000
stub_only=0; [ "${1:-}" = --stub-only ] && stub_only=1

# STUB_CARGO_FLAGS: e.g. --no-default-features (core 1 code in flash)
(cd "$STUB_DIR" && cargo build --release -q ${STUB_CARGO_FLAGS:-})
if [ $stub_only = 0 ]; then
  for f in out/xipImage out/rp2350.dtb; do [ -s $f ] || { echo "load.sh: $f missing; run ./run-build.sh" >&2; exit 1; }; done
  k=$(wc -c < out/xipImage | tr -d ' ')
  [ "$k" -le $((KERNEL_MAX)) ] || { echo "load.sh: xipImage is $k bytes, more than $((KERNEL_MAX))" >&2; exit 1; }
  d=$(wc -c < out/rp2350.dtb | tr -d ' ')
  [ "$d" -le $((0x103FF000 - DTB_ADDR)) ] || { echo "load.sh: DTB too big" >&2; exit 1; }
  [ -s out/rootfs.squashfs ] || { echo "load.sh: out/rootfs.squashfs missing; run ./run-build.sh rootfs" >&2; exit 1; }
  r=$(wc -c < out/rootfs.squashfs | tr -d ' ')
  [ "$r" -le $((ROOTFS_MAX)) ] || { echo "load.sh: rootfs.squashfs is $r bytes, more than $((ROOTFS_MAX))" >&2; exit 1; }
  [ -s out/xip.romfs ] || { echo "load.sh: out/xip.romfs missing; run ./run-build.sh rootfs" >&2; exit 1; }
  x=$(wc -c < out/xip.romfs | tr -d ' ')
  [ "$x" -le $((XIP_MAX)) ] || { echo "load.sh: xip.romfs is $x bytes, more than $((XIP_MAX))" >&2; exit 1; }
fi

in_bootsel() {
  local o; o=$(picotool info 2>&1) && return 0
  case $o in *"No accessible RP-series devices in BOOTSEL mode"*) return 1;; *) return 0;; esac
}
wait_bootsel() { for _ in $(seq 1 "$1"); do in_bootsel && return 0; sleep 1; done; return 1; }
request_bootsel() {
  local port
  # the native console: the 1200-baud touch
  for port in $(ls /dev/cu.usbmodemnative* 2>/dev/null); do
    if lsof "$port" >/dev/null 2>&1; then
      echo "load.sh: $port is open in another program; close it and run again" >&2; exit 1
    fi
    stty -f "$port" 1200 2>/dev/null || true
    wait_bootsel 5 && return 0
  done
  # any other firmware, over SWD: watchdog SCRATCH0 = BOOTSEL magic, then reset
  for chip in RP235x RP235x_riscv; do
    "$P" write --chip $chip --protocol swd b32 0x400d800c 0xb0075e1c >/dev/null 2>&1 || continue
    "$P" reset --chip $chip --protocol swd >/dev/null 2>&1 || true
    wait_bootsel 5 && return 0
  done
  return 1
}
in_bootsel || request_bootsel || { echo "load.sh: the board did not enter BOOTSEL" >&2; exit 1; }
if [ $stub_only = 0 ]; then
  picotool load -v -t bin out/xipImage -o $KERNEL_ADDR
  picotool load -v -t bin out/rp2350.dtb -o $DTB_ADDR
  if picotool verify -t bin out/rootfs.squashfs -o $ROOTFS_ADDR >/dev/null 2>&1; then
    echo "load.sh: the board already has this rootfs.squashfs"
  else
    picotool load -v -t bin out/rootfs.squashfs -o $ROOTFS_ADDR
  fi
  if picotool verify -t bin out/xip.romfs -o $XIP_ADDR >/dev/null 2>&1; then
    echo "load.sh: the board already has this xip.romfs"
  else
    picotool load -v -t bin out/xip.romfs -o $XIP_ADDR
  fi
fi
picotool load -v -x -t elf "$STUB"
echo "load.sh: started; console: screen /dev/cu.usbmodemnative1 115200 (it appears in a few seconds)"
