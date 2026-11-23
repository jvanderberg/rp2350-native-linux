#!/usr/bin/env bash
# busybox-atrans.sh OUT: builds /usr/bin/busybox-atrans, busybox as an ATRANS
# program (atrans-gcc on PATH), into the tree OUT, with the list of its
# applets in OUT/usr/share/busybox-atrans.links. Its config: Buildroot's
# busybox-minimal.config, then the native userland's busybox.fragment (the FDPIC
# busybox's applets), then busybox-atrans.config (ash, MMU build, more
# applets). post-build-devices.sh points the
# image's /bin/sh and applet links at it. Runs in the container.
set -euo pipefail
OUT=$1
V=1.38.0
W=/work/atrans-busybox
SRC=/work/dl/busybox/busybox-$V.tar.bz2
CROSS=arm-atrans-linux-musleabi-
CFG="/src/buildroot/package/busybox/busybox-minimal.config /native/busybox.fragment /native/atrans/busybox-atrans.config"
# the tarball the native rootfs build downloaded (../run-build.sh rootfs)
[ -f $SRC ] || { echo "busybox-atrans: $SRC missing; run ./run-build.sh rootfs first" >&2; exit 1; }
stamp=$(cat $SRC.sha256 2>/dev/null; cat $CFG | sha256sum)
if [ ! -f $W/busybox ] || [ "$(cat $W/.stamp 2>/dev/null)" != "$stamp" ]; then
  rm -rf $W
  mkdir -p $W
  tar -xjf $SRC -C $W --strip-components=1
  for p in /src/buildroot/package/busybox/*.patch; do patch -d $W -p1 -s < "$p"; done
  # each file overrides the ones before it: this kconfig ignores a second
  # assignment, so drop a symbol's earlier lines before appending
  : > $W/.config
  for f in $CFG; do
    for s in $(sed -n 's/^\(CONFIG_[A-Z0-9_]*\)=.*/\1/p; s/^# \(CONFIG_[A-Z0-9_]*\) is not set$/\1/p' "$f"); do
      sed -i "/^# $s is not set\$/d; /^$s=/d" $W/.config
    done
    grep -E '^(CONFIG_|# CONFIG_.* is not set$)' "$f" >> $W/.config
  done
  M="CROSS_COMPILE=$CROSS ARCH=arm SKIP_STRIP=y"
  { yes '' || true; } | make -C $W $M oldconfig >/dev/null
  for s in CONFIG_ASH CONFIG_FEATURE_SH_STANDALONE CONFIG_STATIC; do
    grep -qx "$s=y" $W/.config || { echo "busybox-atrans: $s is not set" >&2; exit 1; }
  done
  ! grep -qx 'CONFIG_NOMMU=y' $W/.config || { echo "busybox-atrans: CONFIG_NOMMU is set" >&2; exit 1; }
  # 32 KiB of stack instead of the toolchain's 128 KiB: every login shell
  # and applet process has a block, and the stack region is part of it
  make -C $W $M CONFIG_EXTRA_CFLAGS="-Os" CONFIG_EXTRA_LDFLAGS="-Wl,--defsym=__atrans_stack=0x8000" \
    -j"$(nproc)" busybox busybox.links >/dev/null
  echo "$stamp" > $W/.stamp
fi
install -D -m 755 $W/busybox "$OUT/usr/bin/busybox-atrans"
install -D -m 644 $W/busybox.links "$OUT/usr/share/busybox-atrans.links"
echo "busybox-atrans: $(wc -c < $W/busybox) bytes unstripped, $(wc -l < $W/busybox.links) applets"
