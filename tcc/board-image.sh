#!/bin/sh
# board-image.sh [ROOT]: inside rp2350-native-tcc (run-host.sh), after
# native-build.sh: a romfs image, out/tcc-romfs.img, for testing a tcc
# build on the board without rebuilding the root file system.
# It holds the tcc tree (ROOT, default /native/out/tcc-root), the repo's
# tcc tests (test/), the sources and build scripts of extras/games, maze,
# samples and pulse with lib's headers (laid out as in the repo so their
# ../lib includes work) and setup.sh, which
# mounts the romfs and links the tree into place (/usr/bin/tcc, cc,
# /usr/lib/tcc, /usr/include, /usr/lib/*). board-load.sh writes it to
# flash at 0x10a00000; the kernel needs an "mtd-rom" node there whose MTD device
# is named "tcc" (linux,mtd-name).
set -eu
ROOT=${1:-/native/out/tcc-root}
IMG=/native/out/tcc-romfs.img
MAX=$((0x5f0000))
W=/work/tcc-romfs
rm -rf $W; mkdir -p $W
cp -a "$ROOT/." $W/
mkdir $W/test $W/extras
cp /native/tcc/test/*.c /native/tcc/test/*.h \
	/native/tcc/test/*.expect /native/tcc/test/run.sh $W/test/
# sources only: /native is the live repo, where a host build may have left
# binaries
mkdir $W/extras/lib
cp /native/extras/lib/*.h $W/extras/lib/
for d in games maze samples pulse; do
	mkdir $W/extras/$d
	cp /native/extras/$d/*.c /native/extras/$d/build.sh $W/extras/$d/
	for h in /native/extras/$d/*.h; do
		if [ -e "$h" ]; then cp "$h" $W/extras/$d/; fi
	done
done
# the same tests built on the host, to tell the board's compiler from the
# board's run time: by the cross tcc (test/x-tcc) and by the userland's
# gcc (test/x-gcc), if they are there
. /native/tcc/env.sh
G=/work/atrans-br/host/bin/arm-atrans-linux-musleabi-gcc
mkdir $W/test/x-tcc $W/test/x-gcc
for t in hello ll fp misc arith; do
	[ -x $XB/armv7m-tcc ] && $XTCC -o $W/test/x-tcc/$t /native/tcc/test/$t.c -lm
	[ -x $G ] && $G -O1 -o $W/test/x-gcc/$t /native/tcc/test/$t.c -lm
done
cat > $W/setup.sh <<'EOF'
#!/bin/sh
# setup.sh: mount the tcc romfs and link its tree into the root file system
M=/mnt/tcc
mkdir -p $M
# the MTD device named "tcc" (/proc/mtd: mtdN: size erasesize "name")
d=$(sed -n 's/^\(mtd[0-9]*\):.*tcc"$/\1/p' /proc/mtd | head -n 1)
[ -n "$d" ] || { cat /proc/mtd; echo "setup: no tcc MTD device" >&2; exit 1; }
grep -q " $M " /proc/mounts || mount -t romfs $d $M || exit 1
mkdir -p /usr/bin /usr/lib
ln -sf $M/usr/bin/tcc /usr/bin/tcc
ln -sf tcc /usr/bin/cc
[ -e /usr/lib/tcc ] || ln -s $M/usr/lib/tcc /usr/lib/tcc
[ -e /usr/include ] || ln -s $M/usr/include /usr/include
for f in $M/usr/lib/*; do
	[ -e "/usr/lib/${f##*/}" ] || ln -s "$f" /usr/lib/
done
rm -rf /tmp/test /tmp/extras
cp -r $M/test $M/extras /tmp/
echo "tcc ready: /tmp/test (sh board-run.sh), /tmp/extras (games/build.sh ...)"
EOF
cp /native/tcc/board-run.sh /native/tcc/board-wv.c $W/test/
chmod 755 $W/setup.sh $W/test/run.sh $W/test/board-run.sh \
	$W/extras/games/build.sh $W/extras/samples/build.sh $W/extras/pulse/build.sh
genromfs -f $IMG -d $W -V tcc
n=$(stat -c %s $IMG)
ls -l $IMG
[ "$n" -le $MAX ] || { echo "board-image: $n bytes, more than $MAX" >&2; exit 1; }
