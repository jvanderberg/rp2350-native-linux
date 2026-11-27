#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# subset-fonts.sh: make fonts/*.ttf, the TrueType fonts the root filesystem
# has in /usr/share/fonts/dejavu for x11.h's x_font_ttf: DejaVu Sans and
# DejaVu Sans Mono, regular and bold, cut down to the characters programs
# here draw (Latin-1 and Latin Extended-A, Greek, Cyrillic, punctuation,
# arrows, maths, shapes and symbols; for the mono fonts also box drawing
# and blocks, for terminals), without hinting, which x11.h doesn't
# use. About 62 KB each instead of 330-760 KB.
# Needs curl and python3 (fontTools goes in a temporary venv).
set -euo pipefail
cd "$(dirname "$0")/.."
VER=2.37
SHA=fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7   # as Buildroot's dejavu.hash
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
curl -sSfL -o "$W/dejavu.tar.bz2" \
  "https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_${VER//./_}/dejavu-fonts-ttf-$VER.tar.bz2"
echo "$SHA  $W/dejavu.tar.bz2" | (sha256sum -c - 2>/dev/null || shasum -a 256 -c -) >/dev/null
tar -C "$W" -xjf "$W/dejavu.tar.bz2"
python3 -m venv "$W/venv"
"$W/venv/bin/pip" -q install fonttools==4.60.2
COMMON=U+0020-007E,U+00A0-017F,U+0192,U+02C6-02DC,U+0391-03C9,U+0401-045F,U+2010-2027,U+2030-203A
COMMON=$COMMON,U+2044,U+20AC,U+2116,U+2122,U+2190-2199,U+21B5,U+2212,U+2219,U+221A,U+221E,U+2248
COMMON=$COMMON,U+2260-2265,U+2302,U+2318,U+2325,U+2387,U+23CE,U+23CF,U+25A0-25FF,U+2600-2613
COMMON=$COMMON,U+2639-263C,U+2660-266F,U+2713-2718,U+FFFD
MONO=$COMMON,U+2500-259F
mkdir -p fonts
for f in DejaVuSans DejaVuSans-Bold DejaVuSansMono DejaVuSansMono-Bold; do
  case $f in *Mono*) u=$MONO ;; *) u=$COMMON ;; esac
  "$W/venv/bin/pyftsubset" "$W/dejavu-fonts-ttf-$VER/ttf/$f.ttf" --unicodes="$u" --no-hinting \
    --layout-features='' --drop-tables+=GSUB,GPOS,GDEF,kern,FFTM,DSIG,MATH \
    --name-IDs=0,1,2,4,6,13,14 --output-file="fonts/$f.ttf"
done
cp "$W/dejavu-fonts-ttf-$VER/LICENSE" fonts/LICENSE
ls -l fonts
