#!/bin/sh
# tests2.sh [TEST...]: tinycc's tests/tests2 with the cross tcc and
# qemu-arm, inside rp2350-native-tcc (run-host.sh). Without -run (a cross
# compiler cannot), each test is built and then run; the tests that need
# -run (-dt snippets), bounds checking, backtraces, shared libraries,
# TLS or another architecture's assembler are skipped (SKIP below), and
# 95_bitfields* as upstream does on 32-bit ARM (type alignment differs).
. /native/tcc/env.sh
TCC=${TCC:-$XTCC}
QEMU=${QEMU:-qemu-arm}
SRC=/tcc-src/tests/tests2
W=/work/tcc-tests2
rm -rf $W; mkdir -p $W; cd $W
SKIP=" 34 85 127 98 99 112 113 114 115 116 117 126 132 148 138 139 140 141
 145 144 146 60 96 125 128 95 141 "
pass=0 fail=0 failed=
tests=${*:-$(cd $SRC && ls [0-9]*_*.c | grep -v '+' | sort -n)}
for f in $tests; do
	f=${f%.c}; f=${f%.test}
	[ -f $SRC/$f.c ] || f=$(cd $SRC && ls ${f}_*.c | head -1) && f=${f%.c}
	n=${f%%_*}
	case "$SKIP" in *" $n "*) continue;; esac
	FLAGS= ARGS=
	case $n in
	22|24) FLAGS=-lm;;
	31) ARGS="arg1 arg2 arg3 arg4 arg5";;
	46) ARGS="[^* ]*[:a:d: ]+\:\*-/: \$ $SRC/46_grep.c";;
	76) FLAGS=-fdollars-in-identifiers;;
	104) FLAGS=$SRC/104+_inline.c;;
	120) FLAGS=$SRC/120+_alias.c;;
	106|124) FLAGS=-pthread;;
	esac
	if [ $n = 46 ]; then
		{ $TCC $FLAGS $SRC/$f.c -o $f.exe && $QEMU ./$f.exe '[^* ]*[:a:d: ]+\:\*-/: $' $SRC/46_grep.c; } 2>&1 \
			| sed -e "s,$SRC/,,g" > $f.output
	else
		{ $TCC $FLAGS $SRC/$f.c -o $f.exe && $QEMU ./$f.exe $ARGS; } 2>&1 \
			| sed -e "s,$SRC/,,g" > $f.output
	fi
	if diff -Nbu $SRC/$f.expect $f.output > $f.diff; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); failed="$failed $f"
		echo "FAIL $f"; head -15 $f.diff
	fi
done
echo "tests2: $pass passed, $fail failed:$failed"
