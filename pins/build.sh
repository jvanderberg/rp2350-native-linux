#!/usr/bin/env bash
# Runs inside the container (see run-build.sh). /native = the repository
# (read-only), /src = src/ (Buildroot), /out = out/, /work = the work volume.
#
# Builds the pin packages with the native userland's own Buildroot
# configuration (buildroot.defconfig + buildroot.fragment, and br-external/
# for libpio, micropython-machine and pins-examples) in
# /work/pins-br, then copies just their files into /out/pins-root.
#   root        build, then make the tree (default)
#   make ARGS   make in the Buildroot output (e.g. make micropython-rebuild)
set -euo pipefail
export BR2_DL_DIR=/work/dl
BR=/work/pins-br
P=/native/pins
JOBS=$(nproc)
# seeded from the native build's tree (its toolchain), as NATIVE_TAG does,
# without the toolchain's build directories (5 GB) but with their stamps
if [[ ! -d $BR && -d /work/arm-br ]]; then
  rsync -a --include='/build/*/.*' --exclude='/build/host-gcc-*/**' \
    --exclude='/build/gcc-final-*/**' --exclude='/build/linux-headers-*/**' \
    --exclude='/build/host-binutils-*/**' /work/arm-br/ "$BR/"
fi
mkdir -p "$BR"

configure() {
  # no post-build scripts: the native ones would copy out/ (this build's
  # own last pins-root among it) into this target, and tree() would then
  # take the MicroPython wrapper for the binary
  { cat /native/buildroot.defconfig $P/buildroot.fragment
    echo 'BR2_ROOTFS_POST_BUILD_SCRIPT=""'; } > $BR/defconfig
  make -C /src/buildroot O=$BR BR2_EXTERNAL=/native/br-external BR2_DEFCONFIG=$BR/defconfig defconfig >/dev/null
}

# MicroPython is built with micropython-machine's C modules: rebuild it when
# they change
cmod_check() {
  local mp
  mp=$(cat /native/br-external/package/micropython-machine/cmod/*/* | md5sum)
  [[ "$(cat $BR/.cmod-sum 2>/dev/null)" == "$mp" ]] || make -C $BR micropython-dirclean
  for pkg in libpio micropython-machine pins-examples; do
    # local packages: take the current sources
    [[ -d $BR/build/$pkg-1.0 ]] && make -C $BR $pkg-dirclean
  done
  echo "$mp" > $BR/.cmod-sum
}

# The same PIO programs built with the board's cc (tcc/'s cross tcc,
# when its build is in the volume) against the board's ATRANS sysroot plus
# the libpio below: what `cc prog.c -lpio` makes on the board. Into $T/tcc
# for the board test; skipped without the tcc build. Called from atrans().
tccpio() {
  local XB=/work/tcc-cross-board TI=/out/tcc-root/usr/lib/tcc/include
  local TS=/work/pins-host/tcc-sysroot L=/native/br-external/package/libpio/src t
  if [[ ! -x $XB/armv7m-tcc || ! -d $TI || ! -d /work/tcc-sysroot-board ]]; then
    echo "pins: no cross tcc ($XB): no tcc-built PIO tests" >&2
    return 0
  fi
  rm -rf $TS; cp -a /work/tcc-sysroot-board $TS; cp -a $A/. $TS/
  mkdir -p $T/tcc
  for t in $L/examples/pio-fifo.c $L/examples/squarewave.c $P/tests/atrans/atrans-pio.c; do
    $XB/armv7m-tcc -B$XB/B -nostdinc -I$TI -I$TS/include -nostdlib -o $T/tcc/$(basename $t .c) $t \
      $TS/lib/crt1.o $TS/lib/crti.o -L$TS/lib -lpio -lc $TS/lib/libgcc.a -lc $TS/lib/crtn.o
  done
}

# PIO for ATRANS programs (the board's cc, atrans/): libpio as a
# static library with its headers in the board's ATRANS sysroot
# (/usr/lib/atrans, where cc looks), and the PIO examples and atrans-pio
# built with the userland's arm-atrans-linux-musleabi-gcc. Skipped when that
# toolchain (atrans/run-build.sh, /work/atrans-br) isn't there.
atrans() {
  local AGCC=/work/atrans-br/host/bin/arm-atrans-linux-musleabi-gcc
  local L=/native/br-external/package/libpio/src A=$R/usr/lib/atrans
  local AO=/work/pins-host/atrans AE=$R/usr/share/examples/pio/atrans ex
  if [[ ! -x $AGCC ]]; then
    echo "pins: no ATRANS toolchain ($AGCC): no ATRANS libpio" >&2
    return 0
  fi
  mkdir -p $A/include $A/lib $AO $AE
  $AGCC -Os -Wall -I/native/linux/drivers/rp2350 -c -o $AO/pio.o $L/pio.c
  $AGCC -Os -Wall -c -o $AO/pioasm.o $L/pioasm.c
  rm -f $A/lib/libpio.a
  ${AGCC%gcc}ar rcs $A/lib/libpio.a $AO/pio.o $AO/pioasm.o
  cp $L/pio.h $A/include/
  cp /native/linux/drivers/rp2350/rp2350-pio.h $A/include/
  for ex in squarewave pio-fifo pio-dma pio-edges; do
    $AGCC -Os -Wall -I$A/include -o $AE/$ex $L/examples/$ex.c -L$A/lib -lpio
  done
  $AGCC -Os -Wall -I$A/include -I$H -o $AE/ws2812 $L/examples/ws2812.c -L$A/lib -lpio
  for t in $P/tests/atrans/*.c; do
    $AGCC -Os -Wall -I$A/include -o $T/$(basename $t .c) $t -L$A/lib -lpio
  done
  # the userland's contract check on the unstripped files (it needs their
  # mapping symbols), for the record; ATRANS_CHECK= another copy of it
  rm -rf $AO/u; mkdir -p $AO/u; cp $AE/* $T/atrans-pio $AO/u/
  OBJDUMP=${AGCC%gcc}objdump READELF=${AGCC%gcc}readelf \
    python3 ${ATRANS_CHECK:-/native/atrans/check-atrans.py} $AO/u/* > /out/pins-atrans-check.txt || true
  tccpio
  ${AGCC%gcc}strip $AE/* $T/atrans-pio
}

tree() {
  local R=/out/pins-root S=$BR/staging CC
  CC=$(ls $BR/host/bin/arm-*-gcc | head -1)
  rm -rf $R; mkdir -p $R
  # every file the pin packages (and their libraries) installed
  awk -F, '$1 ~ /^(libgpiod2|i2c-tools|spidev_test|micropython|micropython-machine|libpio|pins-examples)$/ {print $2}' \
    $BR/build/packages-file-list.txt | sort -u | while read -r f; do
    [[ -e $BR/target/$f || -L $BR/target/$f ]] || continue
    [[ -d $BR/target/$f && ! -L $BR/target/$f ]] && continue
    mkdir -p "$R/$(dirname "$f")"
    cp -a "$BR/target/$f" "$R/$f"
  done
  # the pins-examples programs (the package's own Makefile builds them)
  # are in; now the PIO examples, built as FDPIC programs next to their
  # sources (ATRANS builds of them: atrans() below)
  local H=/work/pins-host E=$R/usr/share/examples/pio T=$R/usr/libexec/pins-tests
  mkdir -p $H $E $T $R/usr/bin
  gcc -O2 -o $H/pioasm -I/native/br-external/package/libpio/src \
    /native/br-external/package/libpio/src/pioasm-main.c \
    /native/br-external/package/libpio/src/pioasm.c \
    /native/br-external/package/libpio/src/pio.c \
    -I/native/linux/drivers/rp2350
  local X=-Os
  for ex in squarewave pio-fifo pio-dma pio-edges; do
    $CC $X -Wall -o $E/$ex /native/br-external/package/libpio/src/examples/$ex.c -lpio
  done
  $H/pioasm /native/br-external/package/libpio/src/examples/ws2812.pio $H/ws2812.pio.h
  $CC $X -Wall -I$H -o $E/ws2812 /native/br-external/package/libpio/src/examples/ws2812.c -lpio
  # extras/ws2812 (the same program) as a command
  mkdir -p $H/x && $H/pioasm /native/extras/ws2812/ws2812.pio $H/x/ws2812.pio.h
  $CC $X -Wall -I$H/x -o $R/usr/bin/ws2812 /native/extras/ws2812/ws2812.c -lpio
  # the PIO self-tests (tests/pio), prebuilt
  $CC $X -Wall -o $T/pio-selftest $P/tests/pio/pio-selftest.c -lpio -lpthread
  cp $P/tests/pio/rp2_selftest.py $T/
  for t in $P/tests/*.c; do
    $CC $X -Wall -o $T/$(basename $t .c) $t
  done
  cp $P/tests/*.sh $T/ 2>/dev/null || true
  ${CC%gcc}strip $E/squarewave $E/pio-fifo $E/pio-dma $E/pio-edges $E/ws2812 \
    $R/usr/bin/ws2812 $T/pio-selftest $T/pinpeek 2>/dev/null || true
  # MicroPython's default heap is 1 MiB of contiguous memory, which nommu
  # may not find after a while: the command runs it with -X heapsize
  if [[ -f $R/usr/bin/micropython ]]; then
    mv $R/usr/bin/micropython $R/usr/bin/micropython.bin
    cat > $R/usr/bin/micropython <<'S'
#!/bin/sh
# MicroPython (unix port, FDPIC) with a heap nommu can find: MICROPY_HEAP
# (default 240K: 256K plus its header would be a 512 KiB block, as nommu
# rounds to powers of two) unless -X heapsize is given.
case " $* " in *" -X heapsize="*) exec /usr/bin/micropython.bin "$@";; esac
exec /usr/bin/micropython.bin -X heapsize=${MICROPY_HEAP:-240K} "$@"
S
    chmod 755 $R/usr/bin/micropython
  fi
  # uClibc's NPTL loads libgcc_s for pthread_exit/pthread_cancel (thread
  # exits in MicroPython's Pin.irq, pio-selftest); ../post-build.sh
  # drops the unstripped 2.7 MB one, stripped it is 100 KB
  mkdir -p $R/lib
  cp $S/lib/libgcc_s.so.1 $R/lib/
  ${CC%gcc}strip $R/lib/libgcc_s.so.1
  cp $P/tests/*.py $T/ 2>/dev/null || true
  atrans
  # i2c-tools' Perl scripts (no Perl here)
  rm -f $R/usr/bin/ddcmon $R/usr/bin/decode-* $R/usr/sbin/i2c-stub-from-dump
  chmod -R u+rwX,go+rX,go-w $R
  (cd $R && find . ! -type d | sort) > /out/pins-root.files
  tar -C $R --owner=0 --group=0 -czf /out/pins-root.tar.gz .
  du -sh $R
}

case "${1:-root}" in
  root) configure; cmod_check; make -C $BR -j"$JOBS"; tree;;
  make) shift; configure; make -C $BR "$@";;
  tree) tree;;
  *) echo "usage: build.sh [root|make ARGS|tree]" >&2; exit 2;;
esac
