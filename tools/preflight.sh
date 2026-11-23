# Source from repository scripts; no work is performed on import.
need_tool() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Missing tool: $1. $2" >&2; exit 1;
  }
}
need_file() {
  [ -s "$1" ] || { echo "Missing or empty file: $1. $2" >&2; exit 1; }
}
need_target() {
  need_tool rustup 'Install Rust with rustup.'
  rustup target list --installed | grep -qx "$1" || {
    echo "Missing Rust target: $1. Run: rustup target add $1" >&2; exit 1;
  }
}

# The Docker image the Buildroot builds run in (docker/Dockerfile), and the
# volume that holds their trees (/work in the container). WORK_VOLUME names
# another volume.
BUILD_IMAGE=rp2350-native-build
WORK_VOLUME=${WORK_VOLUME:-rp2350-native-work}

# buildroot_src REPO: the Buildroot checkout in REPO/src/buildroot, cloned
# when missing, at the release pinned in REPO/run-build.sh (BR_TAG, BR_REV)
buildroot_src() {
  local repo=$1 tag rev expected actual
  tag=$(sed -n 's/^BR_TAG=//p' "$repo/run-build.sh")
  rev=$(sed -n 's/^BR_REV=//p' "$repo/run-build.sh")
  [ -d "$repo/src/buildroot" ] ||
    git clone --depth 1 --branch "$tag" https://gitlab.com/buildroot.org/buildroot.git "$repo/src/buildroot"
  expected=$(git -C "$repo/src/buildroot" rev-parse "refs/tags/$tag^{commit}") || {
    echo "Buildroot tag $tag is missing; fetch that tag in src/buildroot." >&2; exit 1;
  }
  [ "$expected" = "$rev" ] || {
    echo "Buildroot tag $tag must resolve to $rev; found $expected." >&2; exit 1;
  }
  actual=$(git -C "$repo/src/buildroot" rev-parse HEAD)
  if [ "$actual" != "$expected" ] || [ -n "$(git -C "$repo/src/buildroot" status --porcelain)" ]; then
    echo "src/buildroot must be clean at $tag ($expected); found $actual." >&2; exit 1
  fi
}
