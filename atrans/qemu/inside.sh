#!/bin/sh
# Runs in the atrans-qemu container (test-qemu.sh): ARM ELF executables go to
# qemu-arm through binfmt_misc, so the programs can fork and exec each other
# (and exec the container's native /bin/sh). The CPU is a Cortex-A15 running
# the Thumb-2 code, a stand-in for the M33. /r is out/atrans-root.
set -u
B=/proc/sys/fs/binfmt_misc
[ -e $B/register ] || mount -t binfmt_misc binfmt_misc $B
if [ ! -e $B/atrans-arm ]; then
  printf '%s' ':atrans-arm:M::\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-arm-static:F' > $B/register
  trap 'echo -1 > $B/atrans-arm' EXIT
fi
export QEMU_CPU=cortex-a15
export PATH=/r/usr/bin:/r/usr/sbin:$PATH TERMINFO=/r/usr/share/terminfo HOME=/root
cd /tmp
"$@"
