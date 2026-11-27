#!/bin/sh
# Build Vim one translation unit at a time with the board's TCC.
set -eu
objects=
if [ "$#" -ne 0 ]; then
    if [ "$#" -ne 2 ] || [ "$1" != --objects ]; then
        echo "usage: $0 [--objects DIRECTORY]" >&2
        exit 2
    fi
    objects=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
    if [ -e "$objects" ] || [ -L "$objects" ]; then
        echo "output already exists: $objects" >&2
        exit 1
    fi
fi
cd "$(dirname "$0")"
CC=${CC:-cc}
CFLAGS=${CFLAGS:-}
# Keep objects on the SD card. The board's /tmp is limited to 512 KiB.
build=$(mktemp -d "$PWD/.build.XXXXXX")
trap 'rm -rf "$build"' EXIT
trap 'exit 1' HUP INT TERM

cd src
set --
while IFS= read -r source; do
    unit="$build/${source%.c}"
    echo "  CC $source"
    # Separate preprocessing and code generation so TCC can release
    # its preprocessor memory before compiling the translation unit.
    $CC $CFLAGS -I. -Iproto -Icompat -DHAVE_CONFIG_H -D_GNU_SOURCE \
        -D_FILE_OFFSET_BITS=64 -E "$source" -o "$unit.i"
    $CC $CFLAGS -c "$unit.i" -o "$unit.o"
    rm -f "$unit.i"
    set -- "$@" "$unit.o"
done < sources
if [ -n "$objects" ]; then
    mkdir "$objects"
    mv "$@" "$objects/"
    echo "built objects in $objects"
    exit 0
fi
echo "  LD vim.bin"
TCC_LINK_SCRATCH="$build" $CC $CFLAGS -o "$build/vim.bin" "$@" -lncursesw -lm
mv -f "$build/vim.bin" ../vim.bin
echo "built vim.bin"
