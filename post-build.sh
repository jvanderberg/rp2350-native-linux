#!/bin/sh
# Buildroot post-build (TARGET_DIR is $1): trim the root filesystem, which
# lives in flash beside the kernel, and add the test programs and the
# ATRANS swap scripts.
set -e
# libgcc_s (2.7 MB) is only for C++ exceptions and unwinding; busybox and
# the C programs here don't link it
rm -f "$1"/lib/libgcc_s.so*
# test programs for per-process ATRANS address spaces: static ELF linked in
# the window, freestanding (no libc). forktest: v2 layout (one window,
# atrans.ld); forktest3: the same tests, v3 layout (shared text, atrans-v3.ld)
CC=$(ls "$HOST_DIR"/bin/arm-*-gcc | head -1)
for v in "forktest atrans.ld" "forktest3 atrans-v3.ld"; do
  set -- "$1" $v
  "$CC" -Os -mthumb -mcpu=cortex-m4 -mfloat-abi=soft -mno-fdpic -fno-pic -ffreestanding \
    -fno-builtin -nostdlib -static -Wl,-T,/native/tests/$3 -Wl,--build-id=none \
    -Wl,--no-warn-rwx-segments -o "$1/usr/bin/$2" /native/tests/forktest.c
done
set -- "$1"
# whole-process swapping (rp2350-atrans-swap.c), same build as forktest
"$CC" -Os -mthumb -mcpu=cortex-m4 -mfloat-abi=soft -mno-fdpic -fno-pic -ffreestanding \
  -fno-builtin -nostdlib -static -Wl,-T,/native/tests/atrans.ld -Wl,--build-id=none \
  -Wl,--no-warn-rwx-segments -o "$1/usr/bin/swaptest" /native/tests/swaptest.c
# Buildroot leaves mkswap out of busybox on nommu: the kernel writes the
# same Linux swap signature (rp2350-atrans-swap.c, "mkswap PATH")
mkdir -p "$1/usr/sbin"
cat > "$1/usr/sbin/atrans-mkswap" <<'MKSWAP'
#!/bin/sh
# atrans-mkswap DEVICE|FILE: write a Linux swap signature on it (as mkswap)
# so that it can be swap for ATRANS processes (atrans-swapon).
case $1 in
/*) f=$1;;
?*) f=$(pwd)/$1;;
*) echo "usage: atrans-mkswap DEVICE|FILE" >&2; exit 2;;
esac
echo "mkswap $f" > /proc/atrans-swap && echo "atrans-mkswap: $f"
MKSWAP
chmod +x "$1/usr/sbin/atrans-mkswap"
# swap for ATRANS processes at boot: the SD card's swap partition, or a
# swap file on its FAT partition (S03sd mounts it at /mnt/sd)
cat > "$1/usr/sbin/atrans-swapon" <<'SWAPON'
#!/bin/sh
# atrans-swapon [DEVICE|FILE]: start swapping ATRANS processes to it. With
# no argument: the first block device with a Linux swap signature, else
# /mnt/sd/atrans.swp (made with: dd if=/dev/zero of=/mnt/sd/atrans.swp
# bs=64k count=1024 && atrans-mkswap /mnt/sd/atrans.swp; bs=1M needs a 1 MiB
# kernel buffer, which fragmented memory rarely has). See /proc/atrans-swap.
[ -w /proc/atrans-swap ] || exit 0
grep -q '^swap /' /proc/atrans-swap && exit 0	# already on
is_swap() { [ "$(dd if="$1" bs=1 skip=4086 count=10 2>/dev/null)" = SWAPSPACE2 ]; }
on() { echo "$1" > /proc/atrans-swap && echo "atrans-swapon: $1"; }
if [ -n "$1" ]; then on "$1"; exit; fi
for d in /sys/class/block/*; do
  n=${d##*/}
  case $n in ram*|loop*|mtdblock*) continue;; esac
  [ -b /dev/$n ] && is_swap /dev/$n && on /dev/$n && exit 0
done
f=/mnt/sd/atrans.swp
[ -f $f ] && is_swap $f && on $f
exit 0
SWAPON
chmod +x "$1/usr/sbin/atrans-swapon"
cat > "$1/etc/init.d/S04atransswap" <<'INIT'
#!/bin/sh
# swap for ATRANS processes (rp2350-atrans-swap.c), after S03sd. With less
# than 1.5 MiB free (pages plus the ATRANS pool's free space), a process
# idle for 10 s is swapped out, one a second: idle login sessions go to the
# card before memory runs out, and come back when they are typed into.
[ "$1" = start ] || exit 0
atrans-swapon
if grep -q '^swap /' /proc/atrans-swap 2>/dev/null; then
  echo "age 10" > /proc/atrans-swap
  echo "low 1536" > /proc/atrans-swap
fi
exit 0
INIT
chmod +x "$1/etc/init.d/S04atransswap"
# baseline for the switch cost: the same ping-pong between two FDPIC processes
"$CC" -Os -o "$1/usr/bin/pingpong" /native/tests/pingpong.c
# SRAM text experiment: sramload writes a program into the sram-text MTD
"$CC" -Os -o "$1/usr/bin/sramload" /native/tests/sramload.c
mkdir -p "$1/sram"
# busybox's text goes to SRAM in /sbin/overlay-init (post-build-devices.sh),
# before init starts, so that PID 1 runs it from there too
rm -f "$1/etc/init.d/S01sramtext"
# watchd: the SD card and link watchers (S03sd, S41linkwatch) in one small
# process, with an 8 KiB stack (FDPIC takes its size from PT_GNU_STACK)
"$CC" -Os -Wl,-z,stack-size=8192 -o "$1/usr/sbin/watchd" /native/tests/watchd.c
# mdnsd: answers as HOSTNAME.local and advertises ssh and telnet (S45mdnsd)
"$CC" -Os -Wl,-z,stack-size=8192 -o "$1/usr/sbin/mdnsd" /native/tests/mdnsd.c
# no FDPIC busybox-extra (target/ is kept between builds): busybox-atrans
# has its applets
rm -f "$1/bin/busybox-extra"
# tmpfs is RAM that nothing can reclaim or move, outside the ATRANS pool:
# bound it, so that a big file in /tmp fails with ENOSPC instead of waking
# the OOM killer; big files go on /mnt/sd. (tmpfs is ramfs without an MMU,
# which honours size= through the kernel's fs/ramfs/rp2350-size.inc.) Each
# rebuild starts from the target's fstab of the last one: drop an old size=
sed -i -e 's|^\(tmpfs[[:space:]]*/tmp[[:space:]]*tmpfs[[:space:]]*\)mode=1777\(,size=[0-9a-z%]*\)*|\1mode=1777,size=512k|' \
  -e 's|^\(tmpfs[[:space:]]*/run[[:space:]]*tmpfs[[:space:]]*\)mode=0755\(,size=[0-9a-z%]*\)*|\1mode=0755,size=256k|' \
  -e 's|^\(tmpfs[[:space:]]*/dev/shm[[:space:]]*tmpfs[[:space:]]*\)mode=1777\(,size=[0-9a-z%]*\)*|\1mode=1777,size=256k|' "$1/etc/fstab"
grep -q 'size=512k' "$1/etc/fstab" || { echo "post-build: /tmp size not set in fstab" >&2; exit 1; }
# local testing: anything under out/overlay (not in git) is
# copied into the root filesystem as it is
if [ -d /native/out/overlay ]; then cp -a /native/out/overlay/. "$1"/; fi
