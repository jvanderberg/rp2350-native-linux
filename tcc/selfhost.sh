#!/bin/sh
# selfhost.sh: inside rp2350-native-tcc (run-host.sh), after native-build.sh:
# tcc built by tcc. tcc2 is tcc's source compiled and linked by the board's
# tcc (gcc-built, run under qemu-arm); tcc3 is the same source built by
# tcc2. The two must be identical, and tcc2 must equal what the cross tcc
# (built by the host's gcc) makes from the same source.
set -eu
. /native/tcc/env.sh
ROOT=${ROOT:-/native/out/tcc-root}
Q="qemu-arm -L $ROOT"
W=/work/tcc-selfhost
rm -rf $W; mkdir -p $W; cd $W
cp /tcc-src/*.c /tcc-src/*.h /tcc-src/*.def . 2>/dev/null || true
cp $NB/config.h .
# build CC OUT: tcc as one unit (tcc.c includes the rest)
build() {
	$1 -DONE_SOURCE=1 -I. -c tcc.c -o $2.o
	$1 -o $2 $2.o
}
t0=$(date +%s)
build "$Q $ROOT/usr/bin/tcc" tcc2
t1=$(date +%s)
build "$Q ./tcc2" tcc3
t2=$(date +%s)
build "$XTCC" tccx
ls -l tcc2 tcc3 tccx
echo "tcc2 (by the gcc-built tcc, qemu): $((t1 - t0)) s; tcc3 (by tcc2, qemu): $((t2 - t1)) s"
rc=0
if cmp tcc2 tcc3; then echo "PASS selfhost (tcc2 and tcc3 identical)"; else echo "FAIL tcc2 != tcc3"; rc=1; fi
if cmp tcc2 tccx; then echo "PASS cross == native (tcc2 and tccx identical)"; else echo "FAIL tcc2 != tccx"; rc=1; fi
# and tcc3 still works
cp /native/tcc/test/arith.c /native/tcc/test/arith.expect .
$Q ./tcc3 -o arith3 arith.c && $Q ./arith3 > arith3.out
if cmp -s arith3.out arith.expect; then echo "PASS arith by tcc3"; else echo "FAIL arith by tcc3"; rc=1; fi
exit $rc
