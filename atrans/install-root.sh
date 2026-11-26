#!/usr/bin/env bash
# install-root.sh BR EX ROOT: assembles the ATRANS overlay tree ROOT from the
# Buildroot output BR (packages in BR/target, toolchain in BR/host) and the
# extras tree EX, checks every program against the ATRANS contract, strips
# them, and writes ROOT.tar.gz, the on-board sysroot tarball and the reports
# atrans-check.txt and atrans-sizes.txt next to ROOT.
# Runs in the container.
set -euo pipefail
BR=$1 EX=$2 ROOT=$3
OUT=$(dirname "$ROOT")
TGT=$BR/target
# the real path: gcc -M names the headers by it (staging is a symlink)
STG=$(readlink -f "$BR/staging")
export PATH=$BR/host/bin:$PATH
rm -rf "$ROOT"
mkdir -p "$ROOT"/usr/bin "$ROOT"/usr/sbin "$ROOT"/usr/share "$ROOT"/etc/init.d

# Buildroot packages: every ELF executable under bin/sbin, with the symlinks
# that point at them (dropbearmulti's applets, ...)
for d in bin sbin usr/bin usr/sbin; do
  [ -d "$TGT/$d" ] || continue
  for f in "$TGT/$d"/*; do
    [ -e "$f" ] || continue
    if [ -L "$f" ]; then
      cp -P "$f" "$ROOT/usr/${d#usr/}/"
    elif head -c4 "$f" | grep -q ELF; then
      cp "$f" "$ROOT/usr/${d#usr/}/"
    fi
  done
done
# ncurses' config scripts are host tools; its terminfo compilers and the
# lesskey compiler are not needed on the board (each is ~100-190 KB static)
rm -f "$ROOT"/usr/bin/*-config "$ROOT"/usr/bin/*config6
rm -f "$ROOT"/usr/bin/{tic,infocmp,toe,tabs,captoinfo,infotocap,lesskey}
# dangling links (to programs not taken, e.g. busybox's) go
find "$ROOT" -xtype l -delete
cp -a "$TGT/usr/share/terminfo" "$ROOT/usr/share/"
# lua's modules directory, tmux and nano data if any
for d in lua nano; do [ -d "$TGT/usr/share/$d" ] && cp -a "$TGT/usr/share/$d" "$ROOT/usr/share/"; done
# SSH: Buildroot's S50dropbear, host keys in /home/dropbear so they last
# across reboots
install -m 755 /src/buildroot/package/dropbear/S50dropbear "$ROOT/etc/init.d/S50dropbear"
ln -sfn /home/dropbear "$ROOT/etc/dropbear"
mkdir -p "$ROOT/etc/default"
cat > "$ROOT/etc/default/dropbear" <<'D'
# read by /etc/init.d/S50dropbear
mkdir -p -m 700 /home/dropbear
D

# xauth, the ATRANS tests and busybox (build-extras.sh)
cp -a "$EX/usr" "$ROOT/"
# claude-remote: the script and claude-guest-mcp (Rust, built on the host)
install -m 755 /native/claude-remote/claude-remote "$ROOT/usr/bin/claude-remote"
install -m 755 /out/atrans-cargo/thumbv7m-none-eabi/release/claude-guest-mcp "$ROOT/usr/bin/claude-guest-mcp"

# the builder's umask shows through (terminfo 0600, some programs 0700):
# everything readable by all, directories and executables searchable
chmod -R u+rwX,go+rX,go-w "$ROOT"

# the contract check runs on the unstripped files
find "$ROOT" -type f | sort | xargs python3 /native/atrans/check-atrans.py > "$OUT/atrans-check.txt" || {
  grep FAIL "$OUT/atrans-check.txt" >&2; echo "check-atrans: failures, see $OUT/atrans-check.txt" >&2; exit 1;
}
cat "$OUT/atrans-check.txt"
find "$ROOT" -type f -exec sh -c 'head -c4 "$1" | grep -q ELF' _ {} \; -print | xargs atrans-strip
find "$ROOT" -type f -exec sh -c 'head -c4 "$1" | grep -q ELF' _ {} \; -print | sort | xargs ls -l |
  awk '{printf "%8d  %s\n", $5, $9}' | sed "s|$ROOT||" > "$OUT/atrans-sizes.txt"

# the on-board sysroot for cc (tcc) at /usr/lib/atrans: musl's headers, the
# kernel UAPI headers they and the tests need, ncurses; crt*.o, libc.a,
# libgcc.a, libncursesw.a. Nothing above links dynamically, so .a only.
SR=$ROOT/usr/lib/atrans
mkdir -p "$SR/include" "$SR/lib"
W=$(mktemp -d)
MUSL=$(ls -d "$BR"/build/musl-[0-9]*)
{
  (cd "$MUSL/include" && find . -name '*.h' | sed 's|^\./||')
  (cd "$MUSL/arch/arm" && find bits -name '*.h')
  (cd "$MUSL/arch/generic" && find bits -name '*.h')
  echo bits/alltypes.h; echo bits/syscall.h
  echo curses.h; echo ncurses.h; echo term.h; echo panel.h; echo menu.h; echo form.h
  echo linux/gpio.h; echo linux/i2c-dev.h; echo linux/spi/spidev.h; echo linux/input.h
  echo linux/fb.h; echo linux/if.h; echo linux/random.h; echo linux/limits.h
} | sort -u | sed 's|.*|#include <&>|' > "$W/headers.c"
atrans-gcc -M -E -D_GNU_SOURCE -D_XOPEN_SOURCE_EXTENDED "$W/headers.c" -o "$W/headers.d" 2>/dev/null ||
  atrans-gcc -M -MG -E -D_GNU_SOURCE "$W/headers.c" -o "$W/headers.d"
sed 's/[\\ ]/\n/g' "$W/headers.d" | grep "^$STG/usr/include/" | sort -u | while read -r f; do
  rel=${f#"$STG"/usr/include/}
  mkdir -p "$SR/include/$(dirname "$rel")"
  cp "$f" "$SR/include/$rel"
done
rm -rf "$W"
for f in crt1.o crti.o crtn.o rcrt1.o libc.a libm.a libpthread.a libcrypt.a libdl.a librt.a libutil.a libresolv.a libxnet.a; do
  [ -e "$STG/usr/lib/$f" ] && cp "$STG/usr/lib/$f" "$SR/lib/"
  [ -e "$STG/lib/$f" ] && cp "$STG/lib/$f" "$SR/lib/"
done
for f in libncursesw.a libpanelw.a libmenuw.a libformw.a; do
  [ -e "$STG/usr/lib/$f" ] && cp "$STG/usr/lib/$f" "$SR/lib/"
done
GCCLIB=$(atrans-gcc -print-libgcc-file-name)
cp "$GCCLIB" "$(dirname "$GCCLIB")"/crtbeginT.o "$(dirname "$GCCLIB")"/crtend.o "$SR/lib/"
# the v3 link script atrans-gcc uses by default (build.sh v3_default)
install -m 644 /native/atrans/atrans-v3.ld "$SR/lib/atrans-v3.ld"
# narrow-curses names for builds that say -lncurses
for l in ncurses panel menu form; do ln -sf lib${l}w.a "$SR/lib/lib$l.a"; done
# no debug info on the board (libgcc.a is 10 MB with it)
find "$SR/lib" -type f \( -name "*.a" -o -name "*.o" \) -exec atrans-strip --strip-debug {} +
chmod -R u+rwX,go+rX,go-w "$SR"

tar -C "$ROOT" --owner=0 --group=0 -czf "$OUT/atrans-root.tar.gz" .
tar -C "$SR" --owner=0 --group=0 -czf "$OUT/atrans-sysroot.tar.gz" .
du -sh "$ROOT" "$SR" "$OUT/atrans-root.tar.gz" "$OUT/atrans-sysroot.tar.gz"
