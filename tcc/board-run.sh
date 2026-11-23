#!/bin/sh
# board-run.sh: the checks of test/run.sh for a minimal
# busybox (no awk, cmp or diff), one command at a time (run.sh samples
# VmHWM with a loop of sed processes, which on the board competes
# with the compiler for contiguous memory), printing MemAvailable and the
# uptime as it goes. board-image.sh puts it in the romfs's test/.
cd "$(dirname "$0")" || exit 1
pass=0 fail=0
# (busybox ash on nommu cannot run a shell function in $(...): it would
# have to re-exec itself, so these set variables)
stat() { A=$(sed -n 's/^MemAvailable: *//p' /proc/meminfo); U=$(cut -d' ' -f1 /proc/uptime); }
check() {
	if [ -f "$1.out" ] && [ "$(cat "$1.out")" = "$(cat "$1.expect")" ]; then
		pass=$((pass + 1)); echo "PASS $1"
	else
		fail=$((fail + 1)); echo "FAIL $1"; head -10 "$1.out" 2>/dev/null
	fi
}
for t in hello ll fp misc arith gpio; do
	stat; echo "cc $t.c: MemAvailable $A, uptime $U"
	cc -o "$t" "$t.c" -lm && { "./$t" > "$t.out" 2>&1; echo "rc $?"; }
	check "$t"
done
stat; echo "uptime $U"
cc -c vec.c stats.c && cc -ar rcs libvec.a vec.o stats.o &&
	cc -o lib main.c -L. -lvec && ./lib > lib.out 2>&1
check lib
echo "libvec.a: $(cc -ar t libvec.a | tr '\n' ' ')"
cc -run hello.c > run.out 2>&1; cp hello.expect run.expect; check run
rm -f a.out; cc hello.c && ./a.out > aout.out 2>&1; cp hello.expect aout.expect; check aout
stat; echo "uptime $U, MemAvailable $A"
echo "tcc tests: $pass passed, $fail failed"
[ "$fail" = 0 ]
