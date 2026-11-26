#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# release.sh [VERSION]: after build-all.sh, the release files, in
# out/release/:
#   rp2350-native-linux-VERSION.uf2   everything load.sh writes, for
#       drag-and-drop onto the board's BOOTSEL drive (tools/mkuf2.py); it
#       leaves /home and the board config as they are
#   rp2350-extras-VERSION.zip         extras/ with its programs built by
#       the ATRANS gcc at -O2 (tools/extras-gcc.sh)
#   SHA256SUMS
# VERSION defaults to `git describe --tags --always --dirty`.
# The GitHub workflow (.github/workflows/release.yml) runs this.
set -euo pipefail
cd "$(dirname "$0")/.."
. tools/preflight.sh
V=${1:-$(git describe --tags --always --dirty)}
STUB=boot/target/thumbv8m.main-none-eabihf/release/nativeboot
for f in $STUB out/xip.romfs out/rp2350.dtb out/rootfs.squashfs out/xipImage; do
  need_file $f 'Run ./build-all.sh first.'
done
need_tool python3 'Install Python 3.'
R=out/release
rm -rf $R
mkdir -p $R
# the addresses load.sh writes them at
python3 tools/mkuf2.py $R/rp2350-native-linux-$V.uf2 $STUB \
  out/xip.romfs@0x10200000 out/rp2350.dtb@0x103F8000 \
  out/rootfs.squashfs@0x10400000 out/xipImage@0x10D80000
docker build -q -t $BUILD_IMAGE docker >/dev/null
docker run --rm -v "$PWD/out:/out" -v "$PWD:/native:ro" -v $WORK_VOLUME:/work --user 0 $BUILD_IMAGE bash -c '
    set -e
    trap "chown -R $1 /out" EXIT
    chown builder /work; chown -R builder /out
    su builder -c "/native/tools/extras-gcc.sh /out/release/rp2350-extras-$2.zip"
  ' _ "$(id -u):$(id -g)" "$V" 2>&1 | tee out/extras-gcc.log
if command -v sha256sum >/dev/null; then sum="sha256sum"; else sum="shasum -a 256"; fi
(cd $R && $sum -- *.uf2 *.zip > SHA256SUMS)
ls -l $R
