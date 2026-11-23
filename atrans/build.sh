#!/usr/bin/env bash
# Runs inside the container (see run-build.sh). /src = src/ (Buildroot,
# read-only use), /native = the repository (read-only), /out = out/,
# /work = the work volume.
#
# Stages:
#   toolchain  Buildroot for ATRANS programs -> /work/atrans-br (toolchain in
#              host/, packages in target/)
#   extras     extras/ programs and the tests, cross-compiled -> /work/atrans-extras
#   root       /out/atrans-root (+ /out/atrans-root.tar.gz, /out/atrans-sysroot.tar.gz)
#   all        all three
#   make ARGS  make in the Buildroot output (e.g. make dropbear-rebuild)
set -euo pipefail
JOBS=$(nproc)
export BR2_DL_DIR=/work/dl
# Buildroot's own tree stays untouched: a copy, with ARMv7-M allowed to say
# BR2_USE_MMU (the ATRANS kernel gives these programs fork and ELF loading).
BRSRC=/work/atrans-buildroot
BR=/work/atrans-br
EX=/work/atrans-extras
ROOT=/out/atrans-root
stage=${1:-all}

buildroot_copy() {
  mkdir -p $BRSRC
  rsync -a --delete --exclude .git /src/buildroot/ $BRSRC/
  # select BR2_USE_MMU from ARMv7-M as from the MMU cores
  python3 - $BRSRC/arch/Config.in.arm <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = "config BR2_ARM_CPU_ARMV7M\n\tbool\n\tselect BR2_ARCH_HAS_FDPIC_SUPPORT\n"
assert old in s, "arch/Config.in.arm changed: update atrans/build.sh"
s = s.replace(old, old + "\t# ATRANS (atrans/): Linux with real fork() on ARMv7-M\n\tselect BR2_USE_MMU\n")
open(p, "w").write(s)
EOF
  # static-only is all there is here: lrzsz's objection is a uClibc clash
  # (musl has no error()), htop's dlopen is only for libsensors (disabled)
  sed -i '/depends on !BR2_STATIC_LIBS/d' $BRSRC/package/lrzsz/Config.in $BRSRC/package/htop/Config.in
}

toolchain() {
  buildroot_copy
  mkdir -p $BR
  cp /native/atrans/buildroot.defconfig $BR/defconfig
  make -C $BRSRC O=$BR BR2_DEFCONFIG=$BR/defconfig defconfig
  # every option in the defconfig must have survived (a dependency that
  # fails silently drops a package)
  local missing=0 line
  while IFS= read -r line; do
    case $line in ''|'#'*) continue;; esac
    grep -qxF "$line" $BR/.config || { echo "defconfig option not in .config: $line" >&2; missing=1; }
  done < /native/atrans/buildroot.defconfig
  [ $missing = 0 ]
  make -C $BR -j"$JOBS" toolchain
  v3_default
  make -C $BR -j"$JOBS"
  short_names
}

# ATRANS v3 by default: gcc reads a file named "specs" in its library
# directory on top of its built-in specs; this one gives every final link
# atrans-v3.ld (text and rodata in the shared text window), unless the
# command line has its own -T. ld's built-in script (binutils patch) stays
# the v2 layout, for ld -r and for direct ld use without -T.
v3_default() {
  local gcc=$BR/host/bin/arm-atrans-linux-musleabi-gcc
  install -m 644 /native/atrans/atrans-v3.ld "$($gcc -print-sysroot)/usr/lib/atrans-v3.ld"
  printf '*link:\n+ %%{!r:%%{!shared:%%{!T*:-T %%R/usr/lib/atrans-v3.ld}}}\n\n' \
    > "$(dirname "$($gcc -print-libgcc-file-name)")/specs"
}

# atrans-gcc etc.: the toolchain's defaults already give contract-conforming
# binaries (static, ATRANS v3 layout). The compiler drivers are
# Buildroot's wrapper, which finds the real program from its own name, so
# those get a script; the binutils get symlinks.
short_names() {
  local t
  for t in gcc cc cpp; do
    rm -f $BR/host/bin/atrans-$t
    printf '#!/bin/sh\nexec "$(dirname "$0")/arm-atrans-linux-musleabi-%s" "$@"\n' $t > $BR/host/bin/atrans-$t
    chmod 755 $BR/host/bin/atrans-$t
  done
  for t in ld as ar nm objdump objcopy readelf strip size ranlib addr2line; do
    [ -e $BR/host/bin/arm-atrans-linux-musleabi-$t ] && ln -sf arm-atrans-linux-musleabi-$t $BR/host/bin/atrans-$t
  done
}

extras() {
  short_names
  PATH=$BR/host/bin:$PATH /native/atrans/build-extras.sh "$BR" "$EX"
}

root() {
  /native/atrans/install-root.sh "$BR" "$EX" "$ROOT"
}

case "$stage" in
  toolchain) toolchain;;
  extras) v3_default; extras;;
  root) root;;
  all) toolchain; extras; root;;
  make) shift; buildroot_copy; v3_default; make -C $BR "$@";;
  *) echo "unknown stage $stage" >&2; exit 2;;
esac
