#!/usr/bin/env bash
# Testing only: load native Linux with the pins userland on the board.
# out/pins-root (pins/run-build.sh) plus pins-setup.sh becomes a romfs at
# flash 0x10C00000 (the kernel must be built with PINS_TESTFS=1 for its
# "pins" MTD), then load.sh loads the stub, kernel and DTB.
# On the board:  mkdir -p /pins && mount -t romfs mtd:pins /pins && /pins/pins-setup.sh
set -euo pipefail
cd "$(dirname "$0")/.."
ADDR=0x10C00000
MAX=$((0x200000))
[ -d out/pins-root ] || { echo "load-test.sh: out/pins-root missing; run pins/run-build.sh" >&2; exit 1; }
grep -q pins-test out/rp2350.dtb || { echo "load-test.sh: rebuild the kernel with PINS_TESTFS=1" >&2; exit 1; }
S=$(mktemp -d)
trap 'rm -rf "$S"' EXIT
cp -a out/pins-root/. "$S/"
cp pins/pins-setup.sh "$S/"
chmod 755 "$S/pins-setup.sh"
(cd out/pins-root && find . -mindepth 1 -type d | sed 's|^\./|d |' | sort &&
  find . ! -type d | sed 's|^\./|f |' | sort) > "$S/pins-files"
python3 pins/mkromfs.py "$S" out/pins-test.romfs pins
[ "$(wc -c < out/pins-test.romfs)" -le $MAX ] || { echo "load-test.sh: romfs too big" >&2; exit 1; }

in_bootsel() {
  local o; o=$(picotool info 2>&1) && return 0
  case $o in *"No accessible RP-series devices in BOOTSEL mode"*) return 1;; *) return 0;; esac
}
if ! in_bootsel; then
  for port in $(ls /dev/cu.usbmodemnative* 2>/dev/null); do
    stty -f "$port" 1200 2>/dev/null || true
    for _ in 1 2 3 4 5; do in_bootsel && break 2; sleep 1; done
  done
fi
in_bootsel || { echo "load-test.sh: the board did not enter BOOTSEL (try ./load.sh first)" >&2; exit 1; }
if picotool verify -t bin out/pins-test.romfs -o $ADDR >/dev/null 2>&1; then
  echo "load-test.sh: the board already has this romfs"
else
  picotool load -v -t bin out/pins-test.romfs -o $ADDR
fi
exec ./load.sh
