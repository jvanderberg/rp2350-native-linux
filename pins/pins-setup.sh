#!/bin/sh
# Testing only: the romfs of out/pins-root (load-test.sh) mounted at /pins,
# linked into the initramfs root, so the programs find their libraries and
# MicroPython its modules where the squashfs root has them. The
# initramfs's BusyBox has no find: /pins/pins-files lists the tree
# ("d DIR" lines first, then "f FILE").
#   mkdir -p /pins && mount -t romfs mtd1 /pins && /pins/pins-setup.sh
n=0
while read -r t f; do
  if [ "$t" = d ]; then
    mkdir -p "/$f"
  elif [ ! -e "/$f" ] || [ -L "/$f" ]; then
    ln -sf "/pins/$f" "/$f"
    n=$((n + 1))
  fi
done < /pins/pins-files
echo "pins-setup: $n files linked from /pins"
