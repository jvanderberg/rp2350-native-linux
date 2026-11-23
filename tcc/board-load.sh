#!/usr/bin/env bash
# board-load.sh: write out/tcc-romfs.img (board-image.sh) to
# flash at 0x10a00000, then load and start the kernel (load.sh).
# Hold the board lock (tools/board-lock.sh acquire NAME) around it.
# For testing a tcc build without rebuilding the root file system, with a
# kernel whose device tree has a "tcc" mtd-rom node there (rp2350.dts has
# none): 0x10a00000 and up is inside the flash layout's rootfs and kernel
# slots (rootfs at 0x10400000, kernel at 0x10d80000), which must be
# reloaded afterwards.
# On the board: sh /mnt/tcc/setup.sh after mounting, or
#   mkdir -p /mnt/tcc && mount -t romfs mtd:tcc /mnt/tcc && sh /mnt/tcc/setup.sh
set -euo pipefail
cd "$(dirname "$0")/.."
IMG=out/tcc-romfs.img
ADDR=0x10a00000
END=0x10ff0000
P=${PROBE_RS:-$HOME/git/toolchains/probe-rs-master/bin/probe-rs}
[ -s $IMG ] || { echo "board-load: $IMG missing; run board-image.sh" >&2; exit 1; }
n=$(wc -c < $IMG | tr -d ' ')
[ "$n" -le $((END - ADDR)) ] || { echo "board-load: $IMG too big" >&2; exit 1; }

# as load.sh: the console's 1200-baud touch, else a watchdog reset into
# BOOTSEL through the debug probe
in_bootsel() {
	local o; o=$(picotool info 2>&1) && return 0
	case $o in *"No accessible RP-series devices in BOOTSEL mode"*) return 1;; *) return 0;; esac
}
wait_bootsel() { for _ in $(seq 1 "$1"); do in_bootsel && return 0; sleep 1; done; return 1; }
request_bootsel() {
	local port
	for port in $(ls /dev/cu.usbmodemnative* 2>/dev/null); do
		if lsof "$port" >/dev/null 2>&1; then
			echo "board-load: $port is open in another program" >&2; exit 1
		fi
		stty -f "$port" 1200 2>/dev/null || true
		wait_bootsel 5 && return 0
	done
	for chip in RP235x RP235x_riscv; do
		"$P" write --chip $chip --protocol swd b32 0x400d800c 0xb0075e1c >/dev/null 2>&1 || continue
		"$P" reset --chip $chip --protocol swd >/dev/null 2>&1 || true
		wait_bootsel 5 && return 0
	done
	return 1
}
in_bootsel || request_bootsel || { echo "board-load: the board did not enter BOOTSEL" >&2; exit 1; }
picotool load -v -t bin $IMG -o $ADDR
exec ./load.sh
