#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Store the Wi-Fi network the boot stub's core 1 joins (boot/src/wifi.rs) in
# the board's flash, the 4 KB settings sector at 0x103FF000, then restart
# the board. Asks for the passphrase without echoing it; nothing is written
# to disk on the host but a temporary file, removed at the end.
#   tools/wifi.sh SSID          (empty passphrase: an open network)
#   tools/wifi.sh --forget      (erase the stored network)
# Format: "WIFI", SSID length, passphrase length (u32 LE), SSID[32], passphrase[64].
# Writes only that sector: not /home, the kernel or the root filesystem.
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# = 1 ] || { echo "usage: $0 SSID | --forget" >&2; exit 2; }
WIFI_ADDR=0x103FF000
P=${PROBE_RS:-$HOME/git/toolchains/probe-rs-master/bin/probe-rs}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
if [ "$1" = --forget ]; then
  head -c 4096 /dev/zero > "$T/wifi.bin"
else
  read -rsp "passphrase for $1 (blank for an open network): " PASS; echo
  [ ${#1} -le 32 ] || { echo "SSID longer than 32 bytes" >&2; exit 1; }
  [ ${#PASS} = 0 ] || { [ ${#PASS} -ge 8 ] && [ ${#PASS} -le 63 ]; } || { echo "WPA passphrases are 8..63 characters" >&2; exit 1; }
  # passed on stdin, not argv, so it doesn't show in ps
  printf '%s\n%s' "$1" "$PASS" | python3 -c '
import struct, sys
ssid, pw = sys.stdin.buffer.read().split(b"\n", 1)
blk = struct.pack("<4sII32s64s", b"WIFI", len(ssid), len(pw), ssid, pw)
open(sys.argv[1], "wb").write(blk.ljust(4096, b"\0"))' "$T/wifi.bin"
  unset PASS
fi

# BOOTSEL as load.sh gets it: the console's 1200-baud touch, else a
# watchdog reset into BOOTSEL through the debug probe
in_bootsel() {
  local o; o=$(picotool info 2>&1) && return 0
  case $o in *"No accessible RP-series devices in BOOTSEL mode"*) return 1;; *) return 0;; esac
}
wait_bootsel() { for _ in $(seq 1 "$1"); do in_bootsel && return 0; sleep 1; done; return 1; }
request_bootsel() {
  local port
  for port in $(ls /dev/cu.usbmodemnative* 2>/dev/null); do
    if lsof "$port" >/dev/null 2>&1; then
      echo "wifi.sh: $port is open in another program; close it and run again" >&2; exit 1
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
in_bootsel || request_bootsel || { echo "wifi.sh: the board did not enter BOOTSEL" >&2; exit 1; }
picotool load -v -t bin "$T/wifi.bin" -o "$WIFI_ADDR"
picotool reboot
echo "wifi.sh: stored; the board is restarting (the console shows nativeboot's Wi-Fi line)"
