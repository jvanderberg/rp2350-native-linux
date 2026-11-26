#!/bin/sh
# extras.sh: build the extras/ programs with the cross tcc (their own
# build scripts, with cc and tcc pointing at it), inside rp2350-native-tcc
# (run-host.sh), and run the ones that just print with qemu-arm.
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
for d in games maze pulse samples httpd tm xterm mc; do
	run $d sh $d/build.sh
done
run bbs sh -c 'cd bbs && tcc -DBBSDIR="\"/tmp\"" -o bbs bbs.c'
ls -l games/stackfall games/rain games/asteroids games/life maze/maze \
	pulse/pulse samples/bogomips samples/mandel httpd/httpd mc/mc tm/tm \
	xterm/xterm mc/xmc bbs/bbs 2>&1 | awk '{print $5, $NF}'
echo "--- mandel 20:"; qemu-arm samples/mandel 20 | head -12
echo "--- bogomips:"; timeout 20 qemu-arm samples/bogomips | head -5
echo "extras: $ok built, $bad failed"
