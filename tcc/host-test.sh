#!/bin/sh
# host-test.sh: inside rp2350-native-tcc (run-host.sh), after host-build.sh:
# the repo's tcc tests (tcc/test) built by the
# cross tcc and run with qemu-arm, and every binary checked to be ARMv7-M
# Thumb code (thumbcheck.sh).
set -u
. /native/tcc/env.sh
TCC=${TCC:-$XTCC}
QEMU=${QEMU:-qemu-arm}
T=/work/tcc-ht
rm -rf $T; mkdir -p $T; cp /native/tcc/test/* $T/; cd $T
pass=0 fail=0
check() {
	if cmp -s "$1.out" "$1.expect"; then
		pass=$((pass + 1)); echo "PASS $1"
	else
		fail=$((fail + 1)); echo "FAIL $1"; diff "$1.expect" "$1.out" | head -20
	fi
}
for t in hello ll fp misc arith gpio; do
	$TCC -o $t $t.c -lm && $QEMU ./$t > $t.out 2>&1
	check $t
done
$TCC -c vec.c stats.c && $TCC -ar rcs libvec.a vec.o stats.o &&
	$TCC -o lib main.c -L. -lvec && $QEMU ./lib > lib.out 2>&1
check lib
echo "libvec.a: $($TCC -ar t libvec.a | tr '\n' ' ')"
if TCC_LINK_SCRATCH="$T" $TCC -o mixed hello.c > mixed.out 2>&1; then
	echo "FAIL scratch link accepted source input"
	fail=$((fail + 1))
elif grep -q 'link scratch requires separate object compilation' mixed.out; then
	echo "PASS scratch link rejects source input"
	pass=$((pass + 1))
else
	echo "FAIL scratch link rejected source input unexpectedly"
	cat mixed.out
	fail=$((fail + 1))
fi
for t in hello ll fp misc arith gpio main; do $TCC -c $t.c -o $t.o; done
# executables keep their symbols (and so the mapping symbols) with -g
for t in hello fp; do $TCC -g -o $t.g $t.c -lm; done
# what tcc makes, and its run-time support; the -g executables also
# check the sysroot's C library and libgcc, as linked
if sh /native/tcc/thumbcheck.sh ./*.o ./*.g $XB/B/libtcc1.a \
	$XB/B/runmain.o; then
	pass=$((pass + 1)); echo "PASS thumbcheck"
else
	fail=$((fail + 1)); echo "FAIL thumbcheck"
fi
echo "host tcc tests: $pass passed, $fail failed"
[ "$fail" = 0 ]
