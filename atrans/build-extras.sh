#!/usr/bin/env bash
# build-extras.sh BR OUT: cross-compiles the ATRANS tests and busybox with
# the ATRANS toolchain (atrans-gcc on PATH, BR = its Buildroot output) into
# the tree OUT. The programs in extras/ are not in the image: they are built
# on the board (extras/README.md).
# Runs in the container; the repository is at /native, read-only.
set -euo pipefail
BR=$1 OUT=$2
T=/native/atrans/tests
CC=atrans-gcc
CFLAGS="-Os -Wall -Wno-unused-result -ffunction-sections -fdata-sections"
LDFLAGS="-Wl,--gc-sections"
B=$OUT/build
rm -rf "$OUT"
mkdir -p "$B" "$OUT/usr/bin" "$OUT/usr/libexec/atrans-tests"

cc() { echo "  CC $*" | cut -c1-120; $CC $CFLAGS "$@" $LDFLAGS; }

# xauth (atrans/xauth.c): dropbear runs it to store an X11 forwarding's cookie
cc -o "$OUT/usr/bin/xauth" /native/atrans/xauth.c

# ATRANS tests (tests/README in this directory)
for t in "$T"/*.c; do
  n=$(basename "$t" .c)
  cc -o "$OUT/usr/libexec/atrans-tests/$n" "$t" -lpthread
done
cp "$T/run-tests.sh" "$OUT/usr/libexec/atrans-tests/"

# busybox as an ATRANS program: the shell and interactive applets of the
# native image (busybox-atrans.sh)
/native/atrans/busybox-atrans.sh "$OUT"

rm -rf "$B"
