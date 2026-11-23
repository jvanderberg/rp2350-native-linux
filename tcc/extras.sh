#!/bin/sh
# extras.sh: build the extras/ programs with the cross tcc (their own
# build scripts, with cc and tcc pointing at it), inside rp2350-native-tcc
# (run-host.sh), and run the ones that just print with qemu-arm.
# Programs that need curses link only when the sysroot has libncurses.
set -u
. /native/tcc/env.sh
TCC=${TCC:-$XTCC}
W=/work/tcc-extras
rm -rf $W; mkdir -p $W/bin; cp -r /native/extras $W/src; chmod -R u+w $W/src
for n in cc tcc; do printf '#!/bin/sh\nexec %s "$@"\n' "$TCC" > $W/bin/$n; done
chmod +x $W/bin/*
export PATH=$W/bin:$PATH TMPDIR=$W
cd $W/src
ok=0 bad=0
run() { # NAME CMD...
	n=$1; shift
	if "$@" > $W/$n.log 2>&1; then ok=$((ok + 1)); echo "OK   $n"
	else bad=$((bad + 1)); echo "FAIL $n"; tail -5 $W/$n.log; fi
}
run demos sh demos/build.sh
run mandel cc -Wall -o demos/mandel demos/mandel.c
run httpd cc -Wall -o demos/httpd demos/httpd.c
run mc sh mc/build.sh
run tm sh tm/build.sh
run bbs sh -c 'cd bbs && tcc -DBBSDIR="\"/tmp\"" -o bbs bbs.c'
if [ -f $SYSROOT/lib/libncurses.a ] || [ -f $SYSROOT/usr/lib/libncurses.a ]; then
	run nnn sh nnn/build.sh
	run noice sh -c 'cd noice && cc -Iinclude -o noice noice.c strlcat.c strlcpy.c -lncurses'
else
	echo "(no libncurses in the sysroot: nnn and noice are compiled, not linked)"
	run nnn sh -c 'cd nnn && tcc -O2 -DNORL -DNOX11 -DDIM_FILTERED -Icompat -I../noice/include -c nnn.c'
	run noice sh -c 'cd noice && cc -Iinclude -c noice.c strlcat.c strlcpy.c'
fi
ls -l demos/stackfall demos/rain demos/pulse demos/asteroids demos/bogomips \
	demos/life demos/mandel demos/httpd mc/mc tm/tm bbs/bbs 2>&1 | awk '{print $5, $NF}'
echo "--- mandel 20:"; qemu-arm demos/mandel 20 | head -12
echo "--- bogomips:"; timeout 20 qemu-arm demos/bogomips | head -5
echo "extras: $ok built, $bad failed"
