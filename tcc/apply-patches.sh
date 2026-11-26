#!/usr/bin/env bash
# apply-patches.sh: make tcc/tinycc, the source tree the build scripts
# use: tinycc 70adbb12 (a snapshot from the GitHub mirror, or repo.or.cz
# when that fails; kept in build/) with
# the patches in patches/, in order. 0001-0007 are base patches the armv7m
# series applies on top of: a riscv32 target (0001, 0003-0006), whose
# soft-float code the armv7m generator follows, and two changes to shared
# code the armv7m tcc uses (0002: no 'redefined' warning in system headers;
# 0007: the 'memory full' check). 0008 and up add the armv7m target:
# Thumb-2 for ARMv7-M, soft-float EABI, ATRANS programs.
set -euo pipefail
cd "$(dirname "$0")"
REV=70adbb129760b77e3bdfec447c1f986def763f2c
[ ! -e tinycc ] || { echo "apply-patches: tinycc/ exists; remove it first" >&2; exit 1; }
mkdir -p build
tarball=build/tcc-${REV:0:8}.tar.gz
if [ ! -s "$tarball" ]; then
	curl -fsSL --retry 3 "https://github.com/TinyCC/tinycc/archive/$REV.tar.gz" -o "$tarball.new" ||
		curl -fsSL --retry 3 "https://repo.or.cz/tinycc.git/snapshot/$REV.tar.gz" -o "$tarball.new"
	mv "$tarball.new" "$tarball"
fi
mkdir tinycc
tar xzf "$tarball" -C tinycc --strip-components=1
for p in patches/0*.patch; do
	patch -s -d tinycc -p1 < "$p"
done
echo "tinycc/ ready"
