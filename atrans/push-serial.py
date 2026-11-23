#!/usr/bin/env python3
"""push-serial.py PORT LOCAL REMOTE...: copies files to the board over its
serial console (a logged-in busybox shell), as base64 through uudecode.

  push-serial.py /dev/cu.usbmodemnative1 hello /tmp/hello selftest /tmp/selftest

Each file is xz-compressed, sent as base64 in a here-document with echo
off, decoded with `uudecode` and `xzcat` (busybox), made executable, and
its size checked. For small test programs
when the root filesystem can't be rebuilt; a 50 KB file takes a few seconds.
"""
import base64
import lzma
import os
import re
import select
import sys
import termios
import time
import tty


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd, termios.TCSANOW)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def expect(fd, want, timeout=20.0):
    """Reads until the regex WANT matches; returns the match."""
    buf = b""
    deadline = time.time() + timeout
    while not re.search(want.encode(), buf):
        left = deadline - time.time()
        if left <= 0:
            raise SystemExit("push-serial: timeout waiting for %r; got %r" % (want, buf[-300:]))
        r, _, _ = select.select([fd], [], [], min(left, 0.5))
        if r:
            buf += os.read(fd, 4096)
    return re.search(want.encode(), buf)


def send(fd, data):
    view = memoryview(data)
    while view:
        n = os.write(fd, view[:256])
        view = view[n:]
        time.sleep(0.002)


def main():
    if len(sys.argv) < 4 or len(sys.argv) % 2:
        raise SystemExit(__doc__)
    fd = open_port(sys.argv[1])
    send(fd, b"\nstty -echo; echo @@READY\n")
    expect(fd, "@@READY")
    for local, remote in zip(sys.argv[2::2], sys.argv[3::2]):
        data = open(local, "rb").read()
        # a 64 KiB dictionary: xzcat allocates it in one piece, and nommu
        # busybox gets that from contiguous pages
        xz = lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32,
                           filters=[{"id": lzma.FILTER_LZMA2, "preset": 9 | lzma.PRESET_EXTREME,
                                     "dict_size": 64 * 1024}])
        b64 = base64.encodebytes(xz).decode()
        # in heredocs of 40 lines, each acknowledged: the console drops
        # input that arrives faster than the shell reads it
        lines = ["begin-base64 644 %s.xz\n" % remote] + b64.splitlines(True) + ["====\n"]
        send(fd, ("mkdir -p $(dirname {r}); rm -f {r}.uu\n".format(r=remote)).encode())
        for n, i in enumerate(range(0, len(lines), 40)):
            send(fd, ("cat >> %s.uu <<'@@EOF'\n" % remote).encode())
            for line in lines[i:i + 40]:
                send(fd, line.encode())
            send(fd, ("@@EOF\necho @@C%d.$((%d+1))\n" % (n, n)).encode())
            expect(fd, r"@@C%d\.%d" % (n, n + 1), 60)
        send(fd, ("uudecode -o {r}.xz {r}.uu && rm {r}.uu && xzcat {r}.xz > {r} && rm {r}.xz && "
                  "chmod 755 {r} && echo @@SIZE $(wc -c < {r})@@\n").format(r=remote).encode())
        size = expect(fd, r"@@SIZE *(\d+)@@", 60).group(1).decode()
        ok = size == str(len(data))
        print("%s -> %s: %s bytes%s" % (local, remote, size, "" if ok else " (expected %d)" % len(data)))
        if not ok:
            return 1
    send(fd, b"stty echo\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
