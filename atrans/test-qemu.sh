#!/usr/bin/env bash
# Host tests of the ATRANS programs under qemu-arm user mode, in Docker.
# Usage: atrans/test-qemu.sh [COMMAND...]
#   default: tests/run-tests.sh ssh (all smoke tests, with an ssh login)
#   e.g.:    atrans/test-qemu.sh tmux -V
# Needs out/atrans-root (atrans/run-build.sh). The container is
# privileged: it registers qemu-arm for ARM ELF files in binfmt_misc (and
# removes the entry when it ends), so that programs can exec each other.
set -euo pipefail
cd "$(dirname "$0")"
[ -d ../out/atrans-root ] || { echo 'Build first: atrans/run-build.sh' >&2; exit 1; }
docker build -q -t atrans-qemu qemu >/dev/null
[ $# -gt 0 ] || set -- /r/usr/libexec/atrans-tests/run-tests.sh ssh
docker run --rm --privileged -v "$PWD/../out/atrans-root:/r:ro" -v "$PWD/qemu:/q:ro" \
  atrans-qemu sh /q/inside.sh "$@"
