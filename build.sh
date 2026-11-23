#!/usr/bin/env bash
# Runs inside the container (see run-build.sh). /src = src/ (Buildroot),
# /native = the repository (read-only), /out = out/, /work = the work volume.
set -euo pipefail
KVER=${KVER:-6.12.116}
JOBS=$(nproc)
export BR2_DL_DIR=/work/dl
# NATIVE_TAG=name gives a build its own Buildroot and kernel trees
# (/work/arm-br-name, /work/arm-linux-VER-name), seeded from the untagged
# ones, so parallel builds don't share a tree. The Buildroot tree is 6.7 GB,
# so a tagged kernel-only build doesn't copy it: it uses the untagged tree's
# toolchain (read only) unless the tag already has a tree of its own.
stage=${1:-all}
BR=/work/arm-br${NATIVE_TAG:+-$NATIVE_TAG}
K=/work/arm-linux-$KVER${NATIVE_TAG:+-$NATIVE_TAG}
if [[ -n ${NATIVE_TAG:-} ]]; then
  if [[ ! -d $BR && $stage == kernel ]]; then
    BR=/work/arm-br
  elif [[ ! -d $BR && -d /work/arm-br ]]; then
    cp -a /work/arm-br "$BR"
  fi
  [[ -d $K || ! -d /work/arm-linux-$KVER ]] || cp -a "/work/arm-linux-$KVER" "$K"
fi

rootfs() {
  mkdir -p $BR
  cp /native/buildroot.defconfig $BR/defconfig
  make -C /src/buildroot O=$BR BR2_DEFCONFIG=$BR/defconfig defconfig
  # Buildroot patches a package once, when it extracts it: a package whose
  # patches/<package>/ changed since this tree built it is extracted again
  local d pkg sum
  for d in /native/patches/*/; do
    pkg=$(basename "$d")
    sum=$(cat "$d"* | sha256sum | cut -d' ' -f1)
    if [[ "$(cat $BR/.patches-$pkg.sum 2>/dev/null)" != "$sum" ]]; then
      make -C $BR $pkg-dirclean
      echo "$sum" > $BR/.patches-$pkg.sum
    fi
  done
  make -C $BR -j"$JOBS"
  cp $BR/images/rootfs.cpio /out/
  cp $BR/images/rootfs.squashfs /out/
}

kernel() {
  local CROSS
  CROSS=$(ls $BR/host/bin/arm-*-gcc | head -1); CROSS=${CROSS%gcc}
  if [[ ! -d $K ]]; then
    mkdir -p "$K" && curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz" | tar xJ -C "$K" --strip-components=1
  fi
  cd "$K"
  /native/linux/apply.sh "$K"
  make ARCH=arm CROSS_COMPILE=$CROSS tinyconfig
  scripts/kconfig/merge_config.sh -m .config /native/linux.fragment /native/linux-devices.fragment /native/linux-pins.fragment \
    ${SRAM_DATA:+/native/linux-sram-data.fragment}
  make ARCH=arm CROSS_COMPILE=$CROSS olddefconfig
  make ARCH=arm CROSS_COMPILE=$CROSS -j"$JOBS" xipImage
  # SRAM below 0x20068000 (the I/O area shared with core 1, then the boot
  # stub's RAM at 0x2006C000) that the kernel image leaves free becomes the
  # sram-text MTD device (tests/sramload.c)
  local sram=0x20000000
  if grep -q '^CONFIG_RP2350_SRAM_DATA=y' .config; then
    sram=$(printf '0x%x' $(( (0x$(awk '$3 == "_end" {print $1}' System.map) + 0xfff) & ~0xfff )))
  fi
  ${CROSS}cpp -nostdinc -I include -undef -x assembler-with-cpp \
    -DSRAM_TEXT_BASE=$sram -DSRAM_TEXT_SIZE=$(printf '0x%x' $((0x20068000 - sram))) \
    ${PINS_TESTFS:+-DPINS_TESTFS} \
    /native/rp2350.dts | scripts/dtc/dtc -I dts -O dtb -o /out/rp2350.dtb -
  cp arch/arm/boot/xipImage vmlinux System.map /out/
  cp .config /out/linux.config
}

case "$stage" in
  rootfs) rootfs;;
  kernel) kernel;;
  all) rootfs; kernel;;
esac
