#!/bin/sh
# native-test.sh [ROOT]: inside rp2350-native-tcc (run-host.sh), after
# native-build.sh: the board's tcc (ROOT, default /native/out/tcc-root)
# run under qemu-arm with ROOT as its file system prefix (qemu maps the
# absolute paths /usr/lib/tcc, /usr/include, ...): the repo's tcc tests
# as run.sh does them (cc, cc -c, tcc -ar, tcc -run, cc && ./a.out).
set -u
. /native/tcc/env.sh
ROOT=${1:-/native/out/tcc-root}
Q="qemu-arm -L $ROOT"
CC="$Q $ROOT/usr/bin/tcc"
T=/work/tcc-nt
rm -rf $T; mkdir -p $T; cp /native/tcc/test/* $T/; cd $T
pass=0 fail=0
check() {
	if cmp -s "$1.out" "$1.expect"; then
		pass=$((pass + 1)); echo "PASS $1"
	else
		fail=$((fail + 1)); echo "FAIL $1"; diff "$1.expect" "$1.out" | head -20
	fi
}
$CC -v
for t in hello ll fp misc arith gpio; do
	$CC -o $t $t.c -lm && $Q ./$t > $t.out 2>&1
	check $t
done
$CC -c vec.c stats.c && $CC -ar rcs libvec.a vec.o stats.o &&
	$CC -o lib main.c -L. -lvec && $Q ./lib > lib.out 2>&1
check lib
echo "libvec.a: $($CC -ar t libvec.a | tr '\n' ' ')"
$CC -run hello.c > run.out 2>&1; cp hello.expect run.expect; check run
rm -f a.out; $CC hello.c && $Q ./a.out > aout.out 2>&1; cp hello.expect aout.expect; check aout
# -run of a program that takes arguments and reads stdin
printf '#include <stdio.h>\nint main(int c, char **v) { char b[64]; fgets(b, sizeof b, stdin); printf("%%d %%s %%s", c, v[1], b); return 0; }\n' > args.c
echo "from stdin" | $CC -run args.c one > args.out 2>&1; echo "2 one from stdin" > args.expect; check args
echo "native tcc tests: $pass passed, $fail failed"
[ "$fail" = 0 ]
