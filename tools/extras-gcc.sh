#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# extras-gcc.sh ZIP: in the build container (tools/release.sh), after
# build-all.sh: build the extras/ programs with ATRANS gcc and bundle their
# sources, data files and binaries.
# Needs the ATRANS toolchain (atrans/run-build.sh) and, for ws2812, the
# pins build's pioasm and ATRANS libpio (pins/run-build.sh).
# bbs isn't built: bbsctl builds it on the board, for the directory it is
# in (BBSDIR).
set -euo pipefail
ZIP=$1
BR=/work/atrans-br
A=$BR/host/bin
PIO=/out/pins-root/usr/lib/atrans
[ -x $A/atrans-gcc ] || { echo "extras-gcc: no ATRANS gcc ($A); run atrans/run-build.sh" >&2; exit 1; }
[ -x /work/pins-host/pioasm ] && [ -f $PIO/lib/libpio.a ] ||
	{ echo "extras-gcc: no pioasm or ATRANS libpio; run pins/run-build.sh" >&2; exit 1; }
W=/work/extras-gcc
rm -rf $W
mkdir -p $W/tmp
cp -r /native/extras $W/extras
chmod -R u+w $W/extras
export PATH=$A:/work/pins-host:$PATH TMPDIR=$W/tmp
cd $W/extras
for d in games maze pulse samples httpd tm xterm mc; do
	CC=atrans-gcc CFLAGS="-O2 -Wall" sh $d/build.sh
done
# wolf3d defines its own bool and false, which C23 (gcc 15's default) has
CC="atrans-gcc -O2 -std=gnu17" sh wolf3d/build.sh
CC="atrans-gcc -O2 -Wall -I$PIO/include -L$PIO/lib" sh ws2812/build.sh

# Reuse the pinned Buildroot recipes and patches for Info-ZIP. The build
# targets stop before installing the tools into Buildroot's rootfs tree.
BR2_DL_DIR=/work/dl make -C "$BR" -j"$(nproc)" zip-build unzip-build
install -D "$BR/build/zip-3.0/zip" zip/zip
install -D "$BR/build/unzip-6.0/unzip" zip/unzip
mkdir -p zip/source/{zip,unzip}
cp /work/dl/zip/zip30.tar.gz zip/source/zip/
cp /work/dl/unzip/unzip_6.0.orig.tar.gz zip/source/unzip/
for package in zip unzip; do
	cp /work/atrans-buildroot/package/"$package"/*.patch "zip/source/$package/"
done

# the binaries: every ELF file in the tree
mapfile -t bins < <(find . -type f -print0 | xargs -0 python3 -c '
import sys
for f in sys.argv[1:]:
    with open(f, "rb") as h:
        if h.read(4) == b"\x7fELF":
            print(f)
' | sort)
[ ${#bins[@]} -gt 0 ] || { echo "extras-gcc: nothing built" >&2; exit 1; }
# the userland's contract check, on the unstripped files (it needs their
# mapping symbols); then strip them
OBJDUMP=$A/arm-atrans-linux-musleabi-objdump READELF=$A/arm-atrans-linux-musleabi-readelf \
	python3 /native/atrans/check-atrans.py "${bins[@]}" > $W/check.txt ||
	{ cat $W/check.txt; echo "extras-gcc: the ATRANS contract check failed" >&2; exit 1; }
$A/arm-atrans-linux-musleabi-strip "${bins[@]}"
printf '%s\n' "${bins[@]}"

mkdir -p "$(dirname "$ZIP")"
python3 - "$ZIP" <<'EOF'
import os, sys, zipfile
out = sys.argv[1]
with zipfile.ZipFile(out + ".new", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk("."):
        dirs.sort()
        for f in sorted(files):
            p = os.path.join(root, f)
            info = zipfile.ZipInfo.from_file(p, "extras/" + os.path.relpath(p, "."))
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(p, "rb") as h:
                z.writestr(info, h.read())
os.replace(out + ".new", out)
print("extras-gcc:", out, os.path.getsize(out), "bytes")
EOF
