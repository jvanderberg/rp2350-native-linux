#!/bin/sh
# Build mc, and xmc (mc in an X11 window, with terminal tabs from ../lib/vt).
# Each source file is compiled on its own and then linked, which keeps
# tcc's peak memory low enough for the board. Everything is rebuilt every
# time: without a real-time clock the board's file times can't be trusted.
#   ./build.sh [name ...]   build the named programs (default: all)
#   CC=gcc ./build.sh       build with another compiler
cd "$(dirname "$0")" || exit 1
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -Wall}

# build NAME SOURCES...
build() {
    name=$1; shift
    tmp=${TMPDIR:-/tmp}/build-$name.$$
    mkdir -p "$tmp" || return 1
    objs=""
    n=0
    for src in "$@"; do
        # numbered, so sources with the same name in two directories don't collide
        n=$((n + 1))
        obj="$tmp/$n.o"
        echo "  CC $src"
        $CC $CFLAGS -D_GNU_SOURCE -c "$src" -o "$obj" || { rm -rf "$tmp"; return 1; }
        objs="$objs $obj"
    done
    # Link to a temporary name, then rename: safe while the old one runs.
    $CC -o "$name.new" $objs && mv "$name.new" "$name"
    rc=$?
    rm -rf "$tmp"
    [ $rc -eq 0 ] && echo "built $name"
    return $rc
}

# not ${*:-...}: busybox hush expands that to nothing
targets="$*"
[ -n "$targets" ] || targets="mc xmc"
status=0
for t in $targets; do
    case $t in
        mc)  build mc main.c screen.c keys.c panel.c panel_draw.c dialog.c menu.c \
                 fileops.c run.c util.c viewer.c usermenu.c || status=1 ;;
        xmc) build xmc xmc.c panel.c fileops.c usermenu.c util.c \
                 ../lib/vt/vt.c ../lib/vt/sb.c ../lib/vt/lz.c ../lib/vt/vtutil.c || status=1 ;;
        *) echo "unknown program: $t" >&2; status=1 ;;
    esac
done
exit $status
