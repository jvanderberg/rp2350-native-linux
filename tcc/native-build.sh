#!/bin/sh
# native-build.sh OUT: inside rp2350-native-tcc (run-host.sh), after
# host-build.sh: tcc for the board, an ATRANS program, installed under
# OUT (default /native/out/tcc-root):
#   /usr/bin/tcc, /usr/bin/cc -> tcc
#   /usr/lib/tcc/        libtcc1.a, runmain.o, libgcc.a, include/ (tcc's)
#   /usr/lib/            crt1.o crti.o crtn.o libc.a (+ empty libm.a &
#                        co) and the sysroot's other static libraries
#   /usr/include/        the C library's and the kernel's headers
# tcc's sources are compiled by gcc (TARGET_CC, default the userland's
# arm-atrans-linux-musleabi-gcc if it is in /work/atrans-br, else
# arm-none-eabi-gcc, with the sysroot's headers), Thumb ARMv7-M
# soft-float, and linked by the cross tcc: the same ATRANS layout (v3 by
# default, see below) as what it will build.
# BOARD_LIBC=/usr/lib/atrans makes a lean tree instead: tcc then uses the
# C library the userland installs there ({include,lib}, libgcc.a too),
# and OUT gets only /usr/bin and /usr/lib/tcc (about 250 KB). LAYOUT=v2 links tcc itself for ATRANS
# v2 instead of v3 (see below).
set -eu
. /native/tcc/env.sh
OUT=${1:-/native/out/tcc-root}
SRC=/tcc-src
if [ -z "${TARGET_CC:-}" ]; then
	TARGET_CC=arm-none-eabi-gcc
	U=/work/atrans-br/host/bin/arm-atrans-linux-musleabi-gcc
	[ -x $U ] && TARGET_CC=$U
fi
B=$NB
BOARD_LIBC=${BOARD_LIBC:-/usr}
if [ "$BOARD_LIBC" = /usr ]; then
	PATHS="--sysincludepaths={B}/include:/usr/include --libpaths={B}:/usr/lib:/lib --crtprefix=/usr/lib"
else
	PATHS="--sysincludepaths={B}/include:$BOARD_LIBC/include --libpaths={B}:$BOARD_LIBC/lib --crtprefix=$BOARD_LIBC/lib"
fi
CF="-mthumb -march=armv7-m -mfloat-abi=soft -Os -fno-unwind-tables
 -fno-asynchronous-unwind-tables -fno-stack-protector -nostdinc
 -isystem $SYSROOT/include -isystem $($TARGET_CC -print-file-name=include)
 -D__linux__ -D__unix__"
rm -rf "$B"; mkdir -p "$B"; cd "$B"
$SRC/configure --cross-prefix="${TARGET_CC%gcc}" --cpu=armv7m --targetos=Linux \
	--prefix=/usr --config-musl --config-predefs=no --config-bcheck=no \
	--config-backtrace=no $PATHS \
	--extra-cflags="$(echo $CF)" >/dev/null
make -s ONE_SOURCE=yes tcc.o libtcc.a
# ATRANS v3 (the default, as for what tcc builds): tcc's text in the
# shared text window, only its data, a 64 KiB stack and its heap in its
# own block. LAYOUT=v2: one block at 0x11020000.
LDF="-Wl,-Tdata=0x11010000"
[ "${LAYOUT:-v3}" = v2 ] && LDF="-Wl,-Ttext=0x11020000"
$XTCC $LDF -o tcc tcc.o libtcc.a
ls -l tcc

rm -rf "$OUT"
mkdir -p "$OUT/usr/bin" "$OUT/usr/lib/tcc/include"
cp tcc "$OUT/usr/bin/tcc"
ln -sf tcc "$OUT/usr/bin/cc"
cp $XB/B/libtcc1.a $XB/B/runmain.o "$OUT/usr/lib/tcc/"
cp $SRC/include/*.h "$OUT/usr/lib/tcc/include/"
if [ "$BOARD_LIBC" != /usr ]; then
	# the C library, libgcc.a included, is the userland's
	du -sh "$OUT/usr/bin/tcc" "$OUT/usr/lib/tcc" "$OUT"
	exit 0
fi
# libgcc: the soft-float and long long helpers; not the unwinder (tcc
# programs have no unwind tables), nor its VFP/iWMMXt register save code
cp "$SYSROOT/lib/libgcc.a" "$B/libgcc.a"
${TARGET_CC%gcc}ar t "$B/libgcc.a" | grep -E '^(unwind-arm|libunwind|pr-support|unwind-c|emutls|_trampoline|_clear_cache)\.o$' \
	| xargs -r ${TARGET_CC%gcc}ar d "$B/libgcc.a"
${TARGET_CC%gcc}strip -g "$B/libgcc.a"
cp "$B/libgcc.a" "$OUT/usr/lib/tcc/libgcc.a"
mkdir -p "$OUT/usr/include"
# the sysroot's static libraries and startup files (libgcc.a is above)
for f in "$SYSROOT"/lib/*.a "$SYSROOT"/lib/crt[1in].o; do
	[ "${f##*/}" = libgcc.a ] || cp -P "$f" "$OUT/usr/lib/"
done
${TARGET_CC%gcc}strip -g "$OUT/usr/lib/libc.a"
if [ -d "$SYSROOT/include/linux/netfilter" ]; then
	# a sysroot with the whole kernel UAPI (whose netfilter headers clash
	# on a case-insensitive file system): every C library header, and the
	# kernel headers install-headers.sh lists, with what they include
	mkdir "$B/musl"
	tar xzf /work/dl/musl/musl-*.tar.gz -C "$B/musl" --strip-components=1 --wildcards '*/include'
	CC="$TARGET_CC $(echo $CF)" sh /native/tcc/install-headers.sh \
		"$B/musl/include" "$SYSROOT/include" "$OUT/usr/include" "$B"
else
	# an already selected set (the userland's atrans-sysroot.tar.gz)
	cp -r "$SYSROOT/include/." "$OUT/usr/include/"
fi
du -sh "$OUT/usr/bin/tcc" "$OUT/usr/lib/tcc" "$OUT/usr/lib/libc.a" "$OUT/usr/include" "$OUT"
