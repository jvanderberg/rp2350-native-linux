#!/bin/sh
# Buildroot post-build, after post-build.sh (TARGET_DIR is $1): the root
# filesystem in flash (squashfs + tmpfs overlay), /home on flash, the
# network, the SD card and the services.
set -e
T=$1

# ---- programs built elsewhere: the image's hook ---------------------------------
# Trees under out/ (seen here as /out) are copied over the root
# filesystem as they are, when present:
#   atrans-root/  ATRANS programs from the userland build (atrans/:
#                 dropbear, tmux, mc, ...), e.g. atrans-root/usr/bin/tmux
#   tcc-root/     the C compiler (tcc/): tcc-root/usr/bin/tcc,
#                 tcc-root/usr/lib/tcc/..., headers
#   pins-root/    the pin tools (pins/): libgpiod tools, i2c-tools,
#                 spidev_test, libpio and pioasm, MicroPython, examples
# Re-run `run-build.sh rootfs` after changing them; load.sh then flashes the
# new squashfs (it flashes it only when it differs from the board's).
# Buildroot keeps target/ between builds, so the files the last build copied
# are listed (devices-hook.list in the output directory) and removed first:
# a tree taken away, or a file dropped from one, leaves the image. (A file
# that replaced one of Buildroot's, such as busybox's less, is then gone
# until that package is reinstalled.)
L=${BASE_DIR:-$T/..}/devices-hook.list
if [ -f "$L" ]; then
  while IFS= read -r f; do rm -f "$T/$f"; done < "$L"
  rm -f "$L"
fi
for d in /out/atrans-root /out/tcc-root /out/pins-root; do
  [ -d "$d" ] || continue
  (cd "$d" && find . ! -type d) >> "$L"
  # --remove-destination: a busybox applet link in the way (less, clear) is
  # replaced; plain cp would write through it, over /bin/busybox.
  # Times and links only: a macOS ACL on a file in out/ comes through the
  # bind mount as a POSIX ACL, and copying it (-a, or --preserve=mode, which
  # carries ACLs) can leave files such as S50dropbear and claude-guest-mcp at
  # mode 0600. Without it each file gets its source mode less the umask.
  umask 022
  cp -R -d --preserve=timestamps,links --remove-destination "$d"/. "$T"/ &&
    echo "post-build-devices: added $d"
done

mkdir -p "$T/overlay" "$T/rom" "$T/home" "$T/mnt/sd" "$T/xip" "$T/sram"

# ---- the shell and the interactive applets: ATRANS ---------------------------------
# Anything that grows has to be movable and swappable: an ATRANS process
# lives in one block that the ATRANS pool can compact and swap can write to
# the SD card, while an FDPIC process's memory stays where it was put. So
# /bin/sh and every applet link go to busybox-atrans (the userland build's
# busybox-atrans.sh: ash, with real fork), and login shells, telnet, SSH and
# tmux sessions, sh -c and the init scripts all run as ATRANS. The FDPIC
# busybox (its text run in place) keeps PID 1 (/sbin/overlay-init, init) and the
# small fixed daemons below. Real programs (less, clear from the userland
# build) are left as they are.
if [ -x "$T/usr/bin/busybox-atrans" ]; then
  keep=" /sbin/init /linuxrc /sbin/getty /bin/login /usr/sbin/telnetd /sbin/udhcpc "
  n=0
  while read -r l; do
    case $keep in *" $l "*) continue;; esac
    if [ -L "$T$l" ]; then
      case $(readlink "$T$l") in *busybox*) ;; *) continue;; esac
    elif [ -e "$T$l" ]; then
      continue
    fi
    mkdir -p "$T${l%/*}"
    ln -sfn /usr/bin/busybox-atrans "$T$l"
    n=$((n + 1))
  done < "$T/usr/share/busybox-atrans.links"
  echo "post-build-devices: $n applet links to busybox-atrans"
fi

# ---- PID 1: the overlay on the squashfs root -------------------------------------
cat > "$T/sbin/overlay-init" <<'S'
#!/bin/busybox sh
# PID 1 on the read-only squashfs root (mtd1, flash at 0x10400000): put a
# tmpfs overlay on top of it, mount /home, move into the overlay and start
# BusyBox init there. Changes outside /home and /root last until reboot;
# the squashfs stays visible at /rom. FDPIC busybox's hush (/bin/sh is the
# ATRANS ash). The overlay's tmpfs is bounded to 1 MiB.
# the kernel starts init without PATH, and uClibc's default lacks /sbin
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
# boot markers in the kernel log, timestamped there (echo is a builtin)
mark() { echo "overlay-init: $1" > /dev/kmsg; }
mark start
# Every exec here costs the text it loads. /bin/mount and the other applet
# links are busybox-atrans, whose 470 KiB of text would come out of the
# squashfs at each exec (nothing keeps it loaded yet), so the commands go to
# the FDPIC busybox, $bb: the squashfs copy until the xip romfs is mounted,
# then the romfs's copy, run in place. Nor is there any $(...): hush on
# nommu runs it in a shell re-executed from the squashfs.
bb=/bin/busybox
mount() { $bb mount "$@"; }
umount() { $bb umount "$@"; }
mkdir() { $bb mkdir "$@"; }
dd() { $bb dd "$@"; }
cp() { $bb cp "$@"; }
rm() { $bb rm "$@"; }
pivot_root() { $bb pivot_root "$@"; }
printf() { $bb printf "$@"; }
grep() { $bb grep "$@"; }
mount -t proc proc /proc
# mtd NAME: sets m to the MTD device named NAME (physmap names its devices
# after the node: "10200000.xip"), or to "".
mtd() {
  m=
  while IFS=': ' read -r dev _ _ name; do
    name=${name#\"}; name=${name%\"}
    case $name in "$1"|*."$1") m=$dev; return;; esac
  done < /proc/mtd
}
# The xip romfs in flash (mtd "xip", mkxiprom.py, load.sh): each file it
# holds is bound over the same path, so shared libraries and big programs
# run their text in place from flash instead of a copy in RAM. Only if its
# stamp matches the one built with this squashfs. Bound over the squashfs
# root first, uClibc's two files first of all, so that the commands from
# the third one on run entirely in place; then over the overlay below. The
# romfs's busybox runs them only if the romfs has one (xiprom.list).
mark "xip romfs"
mtd xip; x=$m; xip=
if [ -n "$x" ] && mount -t romfs -o ro "$x" /xip 2>/dev/null; then
  a=; b=
  read -r a < /xip/.stamp; read -r b < /etc/xiprom.stamp
  if [ -n "$a" ] && [ "$a" = "$b" ]; then
    xip=1
    [ -x /xip/bin/busybox ] && bb=/xip/bin/busybox
    while read -r f; do mount --bind "/xip$f" "$f"; done < /etc/xiprom.list
  else
    umount /xip
    echo "overlay-init: the xip romfs doesn't match this root filesystem; not using it"
  fi
fi
mount -t tmpfs -o mode=0755,size=1m tmpfs /overlay
mkdir /overlay/upper /overlay/work /overlay/root
mount -t overlay -o lowerdir=/,upperdir=/overlay/upper,workdir=/overlay/work overlay /overlay/root
cd /overlay/root
if [ -n "$xip" ]; then
  mount --bind /xip xip
  while read -r f; do mount --bind "xip$f" ".$f"; done < etc/xiprom.list
fi
mark "overlay mounted"
# busybox's text in SRAM (mtd "sram-text", tests/sramload.c), where every
# exec from now on, init's included, runs it in place; if it doesn't fit
# (with the kernel's hot code in SRAM), the romfs copy bound above runs
mark "sram text"
mtd sram-text; s=$m
[ -n "$s" ] && sramload bin/busybox "/dev/$s" >/dev/null &&
  mount -t romfs -o ro "$s" sram && mount --bind sram/busybox bin/busybox
# /home: jffs2 on the flash region at 0x10100000 (the MTD device "home",
# rp2350-flash.c), kept across reboots; /root lives in it as /home/root.
# It is mounted only if it already holds jffs2 (an erase block that starts
# with a node's magic, 0x1985): this script never formats or erases it.
mark home
mtd home; home=$m
[ -n "$home" ] && home=/dev/$home
ok=0
if [ -n "$home" ] && [ -e "$home" ]; then
  printf '\205\031' > /overlay/magic
  i=0
  while [ $i -lt 64 ]; do
    dd if="$home" of=/overlay/block bs=2 count=1 skip=$((i * 8192)) 2>/dev/null
    grep -qF -f /overlay/magic /overlay/block && { ok=1; break; }
    i=$((i + 1))
  done
  rm -f /overlay/magic /overlay/block
  if [ $ok = 1 ]; then
    mount -t jffs2 -o noatime "${home#/dev/}" home || ok=0
  else
    echo "overlay-init: no jffs2 in the /home flash; not touching it"
  fi
fi
if [ $ok = 1 ]; then
  [ -d home/root ] || { mkdir -m 700 home/root && cp -a /root/. home/root/; }
  mount --bind home/root root
  # ash and hush save their history through <file>.<pid>.new: one killed
  # while writing it (a reboot, the OOM killer) leaves it behind. No shell
  # runs yet, so every one left is stale.
  rm -f home/root/.ash_history.*.new home/root/.hush_history.*.new
else
  echo "overlay-init: /home is in RAM"
fi
mark "exec init"
umount /proc
mount --move /dev dev
pivot_root . rom
exec /bin/busybox chroot . /sbin/init
S
chmod 755 "$T/sbin/overlay-init"

# ---- network ------------------------------------------------------------------------
# eth0 is the Wi-Fi radio on core 1 (it joins the network in the Wi-Fi
# settings sector itself, and joins again when the link stays down). DHCP
# starts in the background at once, so boot doesn't wait for a lease.
# Without an MMU, udhcpc -b (which daemonizes by re-executing itself) dies:
# it runs with -f, put in the background by the shell instead.
cat > "$T/etc/network/interfaces" <<'N'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet manual
  up ip link set eth0 up
  up udhcpc -f -R -i eth0 -p /var/run/udhcpc.eth0.pid </dev/null >/dev/null 2>&1 &
  down kill $(cat /var/run/udhcpc.eth0.pid) 2>/dev/null; ip link set eth0 down
N
# When the link comes back after a loss (core 1 rejoined), ask udhcpc to
# renew at once: the lease may have changed, or the network may be another.
# watchd (S42watchd) runs "S41linkwatch event CARRIER" when it changes.
rm -f "$T/etc/init.d/S41linkwatch"
cat > "$T/etc/init.d/S41linkwatch" <<'S'
#!/bin/sh
case "$1" in
event)
  [ "$2" = 1 ] && kill -USR1 "$(cat /var/run/udhcpc.eth0.pid)" 2>/dev/null
  ;;
esac
exit 0
S
chmod 755 "$T/etc/init.d/S41linkwatch"
# One watcher for the SD card and the link (tests/watchd.c): it reads
# their sysfs files every 2 s and runs the scripts' "event" when one
# changes. (Shell loops cost a hush process each, about 130 KiB.)
cat > "$T/etc/init.d/S42watchd" <<'S'
#!/bin/sh
PID=/var/run/watchd.pid
case "$1" in
start)
  watchd 2 /sys/block/mmcblk0/size "/etc/init.d/S03sd event" \
    /sys/class/net/eth0/carrier "/etc/init.d/S41linkwatch event" </dev/null >/dev/null 2>&1 &
  echo $! > $PID
  ;;
stop)
  [ -f $PID ] && kill "$(cat $PID)" 2>/dev/null
  rm -f $PID
  ;;
esac
S
chmod 755 "$T/etc/init.d/S42watchd"

# ---- SD card ------------------------------------------------------------------------
cat > "$T/etc/init.d/S03sd" <<'S'
#!/bin/sh
# The SD card (mmcblk0, rp2350-sd.c: core 1 runs the card and looks for one
# about once a second; the disk's size is 0 while there is none). A card
# that is in at boot is mounted at once; after that watchd (S42watchd) looks
# at its size every 2 s and runs "S03sd event SIZE" when it changes. When a
# card goes in, its
# partitions are read and the first partition, or the whole card if it has
# no partition table, is mounted as FAT at /mnt/sd. When it comes out,
# /mnt/sd is unmounted. A card unmounted by hand stays unmounted until it
# comes out. Linux's own swap needs an MMU; ATRANS swap (atrans-swapon,
# from post-build.sh) takes a swap partition, or else
# /mnt/sd/atrans.swp, and is turned off before the card is let go.
D=mmcblk0
is_swap() {
  [ "$(dd if="$1" bs=1 skip=4086 count=10 2>/dev/null)" = SWAPSPACE2 ]
}
mounted() {
  while read -r _ m _; do [ "$m" = /mnt/sd ] && return 0; done < /proc/mounts
  return 1
}
card_in() {
  partprobe /dev/$D
  for p in /sys/block/$D/$D*; do
    [ -e "$p" ] || continue
    is_swap "/dev/${p##*/}" && [ -x /usr/sbin/atrans-swapon ] && atrans-swapon "/dev/${p##*/}"
  done
  p=/dev/${D}p1
  [ -b "$p" ] || p=/dev/$D
  r=0
  is_swap "$p" || mount -t vfat -o noatime "$p" /mnt/sd || r=1
  # no swap partition: a swap file on the FAT (/mnt/sd/atrans.swp), if any
  [ -x /usr/sbin/atrans-swapon ] && atrans-swapon
  return $r
}
card_out() {
  # Swap off before the card is let go (the swap may be a file on the FAT).
  # "off" swaps every swapped-out process back in, and is refused (EBUSY)
  # if they don't fit; with the card gone (size 0) it kills them instead,
  # since their memory went with it. At shutdown, what doesn't fit is
  # killed ("off force"): init kills it next anyway.
  if [ -w /proc/atrans-swap ]; then
    echo off > /proc/atrans-swap 2>/dev/null ||
      { [ "$1" = stop ] && echo "off force" > /proc/atrans-swap 2>/dev/null; }
  fi
  mounted && umount -l /mnt/sd
  partprobe /dev/$D 2>/dev/null
}
size() {
  read -r size < /sys/block/$D/size
  echo "$size"
}
case "$1" in
start)
  [ -e /sys/block/$D ] || exit 0
  mkdir -p /mnt/sd
  [ "$(size)" != 0 ] && ! mounted && card_in && echo "SD card on /mnt/sd"
  ;;
event)
  if [ "$2" != 0 ]; then
    mounted || card_in
  else
    card_out
  fi
  ;;
stop)
  card_out stop
  ;;
restart)
  "$0" stop
  "$0" start
  ;;
esac
S
chmod 755 "$T/etc/init.d/S03sd"

# ---- services ---------------------------------------------------------------------
# Kernel memory outside the ATRANS pool can't move or be swapped: cap the
# TCP socket buffers (the kernel's defaults allow 128 KiB per socket here,
# twice that in skb overhead).
cat > "$T/etc/init.d/S02memlimits" <<'S'
#!/bin/sh
[ "$1" = start ] || exit 0
echo "4096 16384 65536" > /proc/sys/net/ipv4/tcp_rmem
echo "4096 16384 65536" > /proc/sys/net/ipv4/tcp_wmem
echo 65536 > /proc/sys/net/core/rmem_max
echo 65536 > /proc/sys/net/core/wmem_max
# when the OOM killer has to run after all (swap and the pool's lending
# came up short), it takes the process whose allocation failed, not one of
# the programs already running
echo 1 > /proc/sys/vm/oom_kill_allocating_task
exit 0
S
chmod 755 "$T/etc/init.d/S02memlimits"
# ...and never the daemons that let you back in. The ones that start
# sessions or jobs get -999: the kernel treats it as -1000 (never killed,
# not even as the allocating task: linux/apply.sh, mm/oom_kill.c) but resets
# it to 0 at exec (fs/exec.c), so the logins, ssh commands and cron jobs
# they start don't inherit it; -1000 would be inherited.
cat > "$T/etc/init.d/S99oomadj" <<'S'
#!/bin/sh
[ "$1" = start ] || exit 0
for d in watchd syslogd klogd udhcpc; do
  for p in $(pidof $d); do echo -1000 > /proc/$p/oom_score_adj; done
done
for d in dropbear telnetd crond inetd getty; do
  for p in $(pidof $d); do echo -999 > /proc/$p/oom_score_adj; done
done
exit 0
S
chmod 755 "$T/etc/init.d/S99oomadj"
# no modules here
rm -f "$T/etc/init.d/S11modules"
# rcS: a marker per init script in the kernel log, timestamped there, so a
# slow boot shows which script took the time (dmesg | grep rcS:)
grep -q 'rcS: ' "$T/etc/init.d/rcS" ||
  sed -i 's|^\t\[ ! -f "\$i" \] && continue$|&\n\techo "rcS: $i" > /dev/kmsg 2>/dev/null|' "$T/etc/init.d/rcS"
# SSH: dropbear itself comes from the userland build (atrans-root); its host
# keys go in /home/dropbear, so they last across reboots (-R makes them at
# the first connection). Buildroot's own S50dropbear is kept if it is there.
ln -sfn /home/dropbear "$T/etc/dropbear"
if [ ! -e "$T/etc/init.d/S50dropbear" ]; then
  cat > "$T/etc/init.d/S50dropbear" <<'S'
#!/bin/sh
DB=/usr/sbin/dropbear
[ -x $DB ] || exit 0
PID=/var/run/dropbear.pid
case "$1" in
start)
  mkdir -p -m 700 /home/dropbear
  start-stop-daemon -S -q -p $PID -x "$DB" -- -R -p 22 -P $PID && echo "dropbear: listening on 22"
  ;;
stop)
  start-stop-daemon -K -q -p $PID
  ;;
restart)
  "$0" stop; sleep 1; "$0" start
  ;;
esac
S
  chmod 755 "$T/etc/init.d/S50dropbear"
fi

# ---- terminals: color and mouse -------------------------------------------------
# Telnet logins arrive as vt102 and the console's getty would say vt100, so
# programs would draw without color. Every client this board meets (Terminal,
# iTerm, PuTTY, screen) is an xterm, so a login shell upgrades those names.
# Scripts (TERM=dumb or unset, not a login shell) are left alone.
mkdir -p "$T/etc/profile.d"
cat > "$T/etc/profile.d/term.sh" <<'S'
case "$TERM" in
  vt100|vt102|vt220|linux|xterm) export TERM=xterm-256color ;;
esac
S
cat > "$T/etc/profile.d/oomadj.sh" <<'S'
echo 0 > /proc/$$/oom_score_adj 2>/dev/null
S
# tmux: 256 colors and true color inside, the mouse on (select a pane or
# window, scroll, resize), and a shorter history: scrollback is RAM.
cat > "$T/etc/tmux.conf" <<'S'
set -g default-terminal "tmux-256color"
set -as terminal-features ",xterm-256color:RGB"
set -g mouse on
set -g history-limit 1000
S

# ---- the xip romfs: text that runs in place from flash ------------------------------
# The files in xiprom.list go into out/xip.romfs too (mkxiprom.py), which
# load.sh writes to flash at 0x10200000 (at most 0x1F8000 bytes);
# overlay-init binds them over their copies here when its stamp matches.
python3 /native/mkxiprom.py "$T" /out/xip.romfs /native/xiprom.list 0x1F8000
