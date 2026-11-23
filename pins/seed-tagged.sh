#!/usr/bin/env bash
# In the container, as root: give NATIVE_TAG=$1 its Buildroot tree before
# build.sh copies the whole untagged one (6.7 GB): the same
# tree without the toolchain's build directories, whose stamps stay.
set -euo pipefail
T=/work/arm-br-$1
[[ -d $T ]] && { echo "$T exists"; exit 0; }
rsync -a --include='/build/*/.*' --exclude='/build/host-gcc-*/**' \
  --exclude='/build/gcc-final-*/**' --exclude='/build/linux-headers-*/**' \
  --exclude='/build/host-binutils-*/**' /work/arm-br/ "$T/"
chown -R --reference=/work/arm-br "$T"
du -sh "$T"
