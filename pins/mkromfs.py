#!/usr/bin/env python3
# mkromfs.py DIR IMAGE [VOLUME]: a romfs image of DIR (regular files,
# directories, symlinks), as genromfs makes, for testing out/
# pins-root on the board without the squashfs root (see README.md).
#
# romfs: big-endian words. Superblock "-rom1fs-", size, checksum (the first
# 512 bytes sum to 0), volume name; then 16-byte aligned file headers:
# next (offset | exec bit 8 | type 0-7), spec (a directory's first entry, a
# hard link's target), size, checksum (the header and name sum to 0), name,
# data. Data starts 16-byte aligned, so FDPIC text maps in place.
import os
import struct
import sys

HRD, DIR, REG, LNK = 0, 1, 2, 3
EXEC = 8


def pad16(b):
    return b + b'\0' * (-len(b) % 16)


class Img:
    def __init__(self):
        self.buf = bytearray()
        self.headers = []  # (offset, header + name length)

    def header(self, name, typ, spec, data):
        off = len(self.buf)
        nm = pad16(name.encode() + b'\0')
        self.buf += struct.pack('>IIII', typ, spec, len(data), 0) + nm
        self.headers.append((off, 16 + len(nm)))
        self.buf += pad16(data)
        return off

    def set_word(self, off, v):
        self.buf[off:off + 4] = struct.pack('>I', v)

    def word(self, off):
        return struct.unpack('>I', self.buf[off:off + 4])[0]

    def chain(self, offs):
        # each entry's next field: the following entry's offset, its own type bits kept
        for a, b in zip(offs, offs[1:]):
            self.set_word(a, b | (self.word(a) & 0xf))

    def directory(self, path, self_off, parent_off):
        offs = [self.header('.', HRD, self_off, b''), self.header('..', HRD, parent_off, b'')]
        for name in sorted(os.listdir(path)):
            p = os.path.join(path, name)
            st = os.lstat(p)
            if os.path.islink(p):
                offs.append(self.header(name, LNK, 0, os.readlink(p).encode()))
            elif os.path.isdir(p):
                off = self.header(name, DIR, 0, b'')
                offs.append(off)
                self.set_word(off + 4, self.directory(p, off, self_off))
            elif os.path.isfile(p):
                with open(p, 'rb') as f:
                    data = f.read()
                typ = REG | (EXEC if st.st_mode & 0o111 else 0)
                offs.append(self.header(name, typ, 0, data))
        self.chain(offs)
        return offs[0]


def main():
    src, out = sys.argv[1], sys.argv[2]
    vol = sys.argv[3] if len(sys.argv) > 3 else 'pins'
    img = Img()
    img.buf += b'-rom1fs-' + struct.pack('>II', 0, 0) + pad16(vol.encode() + b'\0')
    root = img.header('.', DIR, 0, b'')
    img.set_word(root + 4, img.directory(src, root, root))
    img.buf += b'\0' * (-len(img.buf) % 1024)
    img.set_word(8, len(img.buf))
    for off, n in img.headers:
        s = sum(struct.unpack('>%dI' % (n // 4), bytes(img.buf[off:off + n]))) & 0xffffffff
        img.set_word(off + 12, -s & 0xffffffff)
    n = min(512, len(img.buf))
    s = sum(struct.unpack('>%dI' % (n // 4), bytes(img.buf[:n]))) & 0xffffffff
    img.set_word(12, -s & 0xffffffff)
    with open(out, 'wb') as f:
        f.write(img.buf)
    print('%s: %d bytes' % (out, len(img.buf)))


main()
