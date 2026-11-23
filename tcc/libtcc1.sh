#!/bin/sh
# libtcc1.sh SRC SYSROOT OUT [CC]: build tcc's run-time support for
# armv7m (libtcc1.a and runmain.o, for tcc -run) into OUT with gcc
# (default arm-none-eabi-gcc), since tcc has no Thumb-2 assembler:
# alloca (assembler), atomics, the __builtin_* helpers, __clear_cache.
set -eu
SRC=$1 SYSROOT=$2 OUT=$3 CC=${4:-arm-none-eabi-gcc}
AR=${CC%gcc}ar
CF="-mthumb -march=armv7-m -mfloat-abi=soft -O2 -fno-unwind-tables
 -fno-asynchronous-unwind-tables -fno-stack-protector -nostdinc
 -isystem $SYSROOT/include -isystem $($CC -print-file-name=include)"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
for f in alloca.S builtin.c stdatomic.c atomic-armv7m.c dsohandle.c armflush.c; do
	$CC $CF -w -c "$SRC/lib/$f" -o "$W/${f%.*}.o"
done
mkdir -p "$OUT"
rm -f "$OUT/libtcc1.a"
$AR rcs "$OUT/libtcc1.a" "$W"/*.o
$CC $CF -c "$SRC/lib/runmain.c" -o "$OUT/runmain.o"
