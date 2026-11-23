#!/bin/sh
# tcctest.sh: inside rp2350-native-tcc (run-host.sh), after native-build.sh:
# tinycc's tests/tcctest.c built by the cross tcc and by arm-none-eabi-gcc
# (Thumb ARMv7-M, the same musl, linked by tcc), both run with qemu-arm;
# the outputs must match (tinycc's "test3" check, with gcc for test.ref).
set -u
. /native/tcc/env.sh
GCC=arm-none-eabi-gcc
W=/work/tcc-tcctest
rm -rf $W; mkdir -p $W; cd $W
cp /tcc-src/tests/tcctest.c /tcc-src/tests/tcctest.h /tcc-src/tests/42test.h . 2>/dev/null
cp $NB/config.h .
$GCC -mthumb -march=armv7-m -mfloat-abi=soft -O0 -w -std=gnu99 -fno-omit-frame-pointer \
	-nostdinc -isystem $SYSROOT/include -isystem $($GCC -print-file-name=include) \
	-D__linux__ -D__unix__ -I. -I/tcc-src -I/tcc-src/include -fno-unwind-tables \
	-fno-asynchronous-unwind-tables -c tcctest.c -o tcctest-gcc.o &&
	$XTCC -o tcctest.gcc tcctest-gcc.o && qemu-arm ./tcctest.gcc > test.ref
$XTCC -w -I. -I/tcc-src -o tcctest.tcc tcctest.c && qemu-arm ./tcctest.tcc > test.out
wc -l test.ref test.out
if diff -u test.ref test.out > test.diff; then
	echo "PASS tcctest (tcc output = gcc output)"
else
	echo "FAIL tcctest"; head -60 test.diff; exit 1
fi
