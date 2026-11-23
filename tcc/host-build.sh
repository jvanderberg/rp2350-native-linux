#!/bin/sh
# host-build.sh: inside the rp2350-native-tcc image (see run-host.sh), build
# the cross tcc (host -> armv7m) from /tcc-src in /work/tcc-cross, and the
# test sysroot in /work/tcc-sysroot if $SYSROOT is not given.
set -eu
. /native/tcc/env.sh
if [ ! -f "$SYSROOT/lib/libc.a" ]; then
	KERNEL_TARBALL=$(ls /work/dl/linux/linux-*.tar.xz | head -1) \
		sh /native/tcc/test-sysroot.sh /work/dl/musl/musl-1.2.6.tar.gz "$SYSROOT"
fi
B=$XB
mkdir -p "$B"
cd "$B"
[ -f config.mak ] || /tcc-src/configure --config-predefs=no >/dev/null
make -s armv7m-tcc ROOT-armv7m="$SYSROOT"
# tcc's own directory for the cross compiler (-B): headers, libtcc1.a
mkdir -p "$B/B"
ln -sfn /tcc-src/include "$B/B/include"
sh /native/tcc/libtcc1.sh /tcc-src "$SYSROOT" "$B/B"
ln -sf libtcc1.a "$B/B/armv7m-libtcc1.a"
echo "cross tcc: $B/armv7m-tcc -B$B/B"
