#!/usr/bin/env bash
# Host wrapper: build the ATRANS toolchain and programs in Docker.
# Usage: atrans/run-build.sh [all|toolchain|extras|root|shell]
#   toolchain  gcc + musl (static, Thumb-2 ARMv7-M, soft-float, the ATRANS
#              v3 layout) and the Buildroot packages (tmux, dropbear, ...)
#   extras     extras/ programs, tests and claude-guest-mcp (needs host Rust)
#   root       out/atrans-root/ and its tarballs
#   shell      a shell in the container with the toolchain on PATH
#   make ARGS  make in the Buildroot output, e.g. make dropbear-dirclean all
# Runs in the rp2350-native-build image with the Buildroot checkout in src/
# (see ../run-build.sh); the trees live in the work volume under
# /work/atrans-*. The busybox source busybox-atrans.sh builds is the one the
# native rootfs build downloads (../run-build.sh rootfs): run that first.
set -euo pipefail
cd "$(dirname "$0")"
. ../tools/preflight.sh
stage=${1:-all}
case "$stage" in all|toolchain|extras|root|shell|make) ;; *)
  echo "Usage: $0 [all|toolchain|extras|root|shell|make ARGS...]" >&2; exit 2;;
esac
for tool in git docker; do need_tool "$tool" 'See README.md.'; done
docker info >/dev/null || { echo 'Start Docker and retry.' >&2; exit 1; }
buildroot_src "$(cd .. && pwd)"
# claude-guest-mcp is Rust; the container has none, so it is built here
if [[ $stage == all || $stage == extras ]]; then
  need_tool cargo 'Install Rust (rustup) to build claude-guest-mcp.'
  rustup target list --installed | grep -qx thumbv7m-none-eabi || rustup target add thumbv7m-none-eabi
  # claude-remote/.cargo/config.toml links it with link-arm.x; kept
  # unstripped for check-atrans.py (install-root.sh strips it)
  (cd ../claude-remote && CARGO_PROFILE_RELEASE_STRIP=false cargo build --release --locked --target thumbv7m-none-eabi \
    --target-dir ../out/atrans-cargo)
fi
mkdir -p ../out
docker build -q -t $BUILD_IMAGE ../docker >/dev/null
docker volume create $WORK_VOLUME >/dev/null
args="$*"
run() {
  docker run --rm "$@" -v "$PWD/../src:/src" -v "$PWD/../out:/out" -v "$PWD/..:/native:ro" \
    -v $WORK_VOLUME:/work --user 0 $BUILD_IMAGE bash -c '
      set -e
      trap "chown -R $1 /out" EXIT
      chown builder /work; chown -R builder /out
      if [ "$2" = shell ]; then  # one word: the stage alone
        su builder -c "PATH=/work/atrans-br/host/bin:\$PATH bash"
      else
        su builder -c "/native/atrans/build.sh $2"  # $2: the stage and its arguments
      fi
    ' _ "$(id -u):$(id -g)" "$args"
}
if [[ $stage == shell ]]; then
  run -it
else
  run 2>&1 | tee "../out/atrans-build-$stage.log"
fi
