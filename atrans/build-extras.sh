#!/usr/bin/env bash
# build-extras.sh BR OUT: cross-compiles the programs in extras/ (plain C that
# tcc also builds) and the ATRANS tests with the ATRANS toolchain
# (atrans-gcc on PATH, BR = its Buildroot output) into the tree OUT.
# Runs in the container; the repository is at /native, read-only.
set -euo pipefail
BR=$1 OUT=$2
X=/native/extras
T=/native/atrans/tests
CC=atrans-gcc
CFLAGS="-Os -Wall -Wno-unused-result -ffunction-sections -fdata-sections"
LDFLAGS="-Wl,--gc-sections"
B=$OUT/build
rm -rf "$OUT"
mkdir -p "$B" "$OUT/usr/bin" "$OUT/usr/share/bbs" "$OUT/usr/libexec/atrans-tests"

cc() { echo "  CC $*" | cut -c1-120; $CC $CFLAGS "$@" $LDFLAGS; }

# demos (extras/demos/build.sh, plus mandel and httpd, which it leaves out)
for d in stackfall rain asteroids bogomips life mandel; do
  cc -o "$OUT/usr/bin/$d" "$X/demos/$d.c" -lm
done
cc -o "$OUT/usr/bin/pulse" "$X/demos/pulse.c" "$X/demos/pulse_draw.c" "$X/demos/pulse_data.c" -lm
# busybox has an httpd applet; the demo one keeps a name of its own
cc -o "$OUT/usr/bin/demo-httpd" "$X/demos/httpd.c"

# mc and tm (their build.sh: each file with -D_GNU_SOURCE)
cc -D_GNU_SOURCE -o "$OUT/usr/bin/mc" $(for f in main screen keys panel dialog menu fileops run util viewer usermenu; do echo "$X/mc/$f.c"; done)
cc -D_GNU_SOURCE -o "$OUT/usr/bin/tm" $(for f in main client server copy vt render sb lz keys proto util; do echo "$X/tm/$f.c"; done)

# nnn and noice against ncursesw (the board's tcc builds use narrow curses
# shims; here the real wide library is there). compat/ has the empty fts.h.
# nnn uses 64-bit atomics: libatomic (locks, no kuser helpers on v7-M).
cc -DNORL -DNOX11 -DDIM_FILTERED -I"$X/nnn/compat" -o "$OUT/usr/bin/nnn" "$X/nnn/nnn.c" -lncursesw -lpthread -latomic
cc -o "$OUT/usr/bin/noice" "$X/noice/noice.c" "$X/noice/strlcat.c" "$X/noice/strlcpy.c" -lncursesw

# the BBS: binary, MUD and settings in /usr/share/bbs (its data/ is made
# there at run time: the overlay keeps it until reboot). No bbs.c, so
# bbsctl uses the binary instead of rebuilding it with tcc.
cc -DBBSDIR='"/usr/share/bbs"' -o "$OUT/usr/share/bbs/bbs" "$X/bbs/bbs.c" -lcrypt
cp -r "$X/bbs/mud" "$X/bbs/bbs.conf" "$X/bbs/welcome.txt" "$X/bbs/bbsctl" "$OUT/usr/share/bbs/"
ln -sf ../share/bbs/bbsctl "$OUT/usr/bin/bbsctl"

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
