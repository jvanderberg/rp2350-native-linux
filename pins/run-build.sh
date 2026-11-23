#!/usr/bin/env bash
# Host wrapper: build the pins userland for native Linux in Docker and put it
# in out/pins-root/ (see README.md here).
# Usage: pins/run-build.sh [root|make ARGS...]
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
. tools/preflight.sh
for tool in git docker; do need_tool "$tool" 'See README.md.'; done
docker info >/dev/null || { echo 'Start Docker and retry.' >&2; exit 1; }
buildroot_src "$REPO"
mkdir -p out
docker build -q -t $BUILD_IMAGE docker >/dev/null
docker volume create $WORK_VOLUME >/dev/null
docker run --rm -v "$REPO/src:/src" -v "$REPO/out:/out" -v "$REPO:/native:ro" \
  -v $WORK_VOLUME:/work --user 0 $BUILD_IMAGE bash -c '
    set -e
    trap "chown -R $1 /out" EXIT
    chown builder /work; chown -R builder /out
    shift
    su builder -c "/native/pins/build.sh $*"
  ' _ "$(id -u):$(id -g)" "${@:-root}" 2>&1 | tee out/pins-build.log
