#!/bin/sh
# import-sysroot.sh TARBALL [OUT]: unpack the userland's sysroot
# (atrans-sysroot.tar.gz from atrans/run-build.sh: include/, lib/ with the
# static musl, libgcc.a and ncurses, the files the board has in
# /usr/lib/atrans) into OUT, default /work/tcc-sysroot-board, which
# env.sh then uses as SYSROOT. In rp2350-native-tcc (run-host.sh), e.g.
#   tcc/run-host.sh sh /native/tcc/import-sysroot.sh \
#	/native/out/atrans-sysroot.tar.gz
# import-sysroot.sh STAGING LIBGCC OUT: the same layout from a Buildroot
# staging directory (lib/ crt*.o libc.a, usr/include, usr/lib's static
# libraries) and its compiler's libgcc.a.
set -eu
if [ $# -le 2 ]; then
	OUT=${2:-/work/tcc-sysroot-board}
	rm -rf "$OUT"; mkdir -p "$OUT"
	tar xzf "$1" -C "$OUT"
	ls "$OUT/lib"
	exit 0
fi
STAGING=$1 LIBGCC=$2 OUT=$3
rm -rf "$OUT"; mkdir -p "$OUT/lib"
cp -r "$STAGING/usr/include" "$OUT/include"
for d in "$STAGING/lib" "$STAGING/usr/lib"; do
	for f in "$d"/*.a "$d"/*.o; do
		[ -f "$f" ] && cp -L "$f" "$OUT/lib/"
	done
done
cp "$LIBGCC" "$OUT/lib/libgcc.a"
ls "$OUT/lib"
