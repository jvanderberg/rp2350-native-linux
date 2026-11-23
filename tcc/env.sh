# env.sh: sourced by the scripts that run in rp2350-native-tcc (run-host.sh).
# SYSROOT  the static musl sysroot (include/, lib/ with crt*.o libc.a
#          libgcc.a): by default the userland's (atrans/'s
#          atrans-sysroot.tar.gz, unpacked by import-sysroot.sh into
#          /work/tcc-sysroot-board) if it is there, else the test one
#          (test-sysroot.sh, /work/tcc-sysroot)
# XB, NB   where the cross tcc and the board's tcc are built: one pair of
#          directories per sysroot
if [ -z "${SYSROOT:-}" ]; then
	SYSROOT=/work/tcc-sysroot
	[ -d /work/tcc-sysroot-board/lib ] && SYSROOT=/work/tcc-sysroot-board
fi
_tag=${SYSROOT##*/}; _tag=${_tag#tcc-sysroot}
XB=${XB:-/work/tcc-cross$_tag}
NB=${NB:-/work/tcc-native$_tag}
XTCC="$XB/armv7m-tcc -B$XB/B"
