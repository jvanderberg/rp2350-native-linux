#!/bin/sh
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")" || exit 1
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -Wall}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/build-xedit.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM

$CC $CFLAGS -c document.c -o "$tmp/document.o" &&
    $CC $CFLAGS -c xedit.c -o "$tmp/xedit.o" &&
    $CC -o xedit.new "$tmp/document.o" "$tmp/xedit.o" &&
    mv xedit.new xedit
