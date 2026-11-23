#!/usr/bin/env python3
"""mkxiprom.py TARGET OUT LIST [MAX]: the xip romfs for flash at 0x10200000.

Copies the files named in LIST (one path per line, relative to the root
filesystem TARGET; '#' starts a comment) into an uncompressed romfs image
OUT, under the same paths. On the board the romfs is an mtd-rom device
("xip", rp2350.dts) and /sbin/overlay-init bind-mounts each file over its
copy in the squashfs. romfs on a ROM MTD maps file data in place on nommu,
so the text of these programs and libraries runs from flash instead of
being copied into PSRAM (MmapCopy).

Also writes TARGET/etc/xiprom.list (the paths) and TARGET/etc/xiprom.stamp
(a hash of the image's files), and puts the same stamp in the romfs as
/.stamp: overlay-init uses the romfs only when the two match, so a stale
or missing image in flash just leaves the squashfs copies in use.

romfs (fs/romfs): big-endian words. Superblock "-rom1fs-", full size,
checksum of the first 512 bytes, volume name. Then 16-byte aligned file
headers: next (offset | exec bit 8 | type), spec (a directory's first
entry), size, checksum (the header and its name sum to 0), name padded to
16. A file's data follows its header.

An ELF file is cut after its last PT_LOAD segment, as tests/sramload.c
does: section headers and symbols are not needed to run it (an unstripped
libuClibc carries 190 KiB of them). Seen through the bind mount, such a
file is the runnable part of the one in the squashfs.
"""
import hashlib
import os
import struct
import sys

DIR, REG = 1, 2
EXEC = 8


def pad16(n):
    return (n + 15) & ~15


def be_sum(b):
    b = b + b"\0" * (-len(b) % 4)
    return sum(struct.unpack(">%dI" % (len(b) // 4), b)) & 0xFFFFFFFF


class Node:
    def __init__(self, name, kind, data=b"", mode=0o644):
        self.name, self.kind, self.data, self.mode = name, kind, data, mode
        self.children = {}
        self.off = 0

    def hdr_len(self):
        return 16 + pad16(len(self.name) + 1)


def elf_cut(data):
    """An ELF file up to the end of its last PT_LOAD; anything else whole."""
    if data[:4] != b"\x7fELF" or data[4] != 1:	# 32-bit only
        return data
    end = "<" if data[5] == 1 else ">"
    phoff, = struct.unpack_from(end + "I", data, 28)
    phentsize, phnum = struct.unpack_from(end + "HH", data, 42)
    top = 0
    for i in range(phnum):
        p_type, p_offset, _, _, p_filesz = struct.unpack_from(end + "IIIII", data, phoff + i * phentsize)
        if p_type == 1:
            top = max(top, p_offset + p_filesz)
    return data[:top] if top else data


def build(root, files):
    top = Node("", DIR)
    for f in files:
        parts = f.strip("/").split("/")
        d = top
        for p in parts[:-1]:
            d = d.children.setdefault(p, Node(p, DIR))
        src = os.path.join(root, f.strip("/"))
        st = os.lstat(src)
        if os.path.islink(src):
            raise SystemExit("mkxiprom: %s is a symlink; list its target" % f)
        with open(src, "rb") as fh:
            d.children[parts[-1]] = Node(parts[-1], REG, elf_cut(fh.read()), st.st_mode)
    return top


def layout(top, stamp):
    # The first header after the superblock is the root directory (named
    # "."); a directory's spec is the offset of its first entry, entries
    # are chained by "next". No "." and ".." entries (the VFS answers
    # those). Files before subdirectories, depth first.
    top.children[".stamp"] = Node(".stamp", REG, stamp)
    top.name = "."
    order = [top]

    def walk(d):
        d.entries = sorted(d.children.values(), key=lambda n: (n.kind == DIR, n.name))
        order.extend(d.entries)
        for n in d.entries:
            if n.kind == DIR:
                walk(n)

    walk(top)
    off = 16 + pad16(len("xip") + 1)
    for n in order:
        if n.kind == REG:	# file data 64-byte aligned (the header before it moves)
            off += -(off + n.hdr_len()) % 64
        n.off = off
        off += pad16(n.hdr_len() + len(n.data))
    return order, (off + 1023) & ~1023


def emit(top, order, total):
    img = bytearray(total)
    img[0:8] = b"-rom1fs-"
    struct.pack_into(">I", img, 8, total)
    img[16:20] = b"xip\0"
    nxt = {}
    for d in order:
        if d.kind == DIR:
            for a, b in zip(d.entries, d.entries[1:]):
                nxt[id(a)] = b.off
    for n in order:
        if n.kind == DIR:
            spec, size, execb = (n.entries[0].off if n.entries else 0), 0, EXEC
        else:
            spec, size = 0, len(n.data)
            execb = EXEC if n.mode & 0o111 else 0
        h = bytearray(n.hdr_len())
        struct.pack_into(">III", h, 0, nxt.get(id(n), 0) | execb | n.kind, spec, size)
        h[16:16 + len(n.name)] = n.name.encode()
        struct.pack_into(">I", h, 12, (-be_sum(bytes(h))) & 0xFFFFFFFF)
        img[n.off:n.off + len(h)] = h
        img[n.off + len(h):n.off + len(h) + len(n.data)] = n.data
    struct.pack_into(">I", img, 12, (-be_sum(bytes(img[:min(512, total)]))) & 0xFFFFFFFF)
    return img


def main():
    root, out, lst = sys.argv[1:4]
    limit = int(sys.argv[4], 0) if len(sys.argv) > 4 else None
    files = []
    for line in open(lst):
        line = line.split("#", 1)[0].strip()
        if line and os.path.exists(os.path.join(root, line.strip("/"))):
            files.append("/" + line.strip("/"))
        elif line:
            print("mkxiprom: %s not in the image, left out" % line)
    h = hashlib.sha256()
    for f in files:
        h.update(f.encode() + b"\0")
        h.update(open(os.path.join(root, f.strip("/")), "rb").read())
    stamp = (h.hexdigest()[:32] + "\n").encode()
    top = build(root, files)
    order, total = layout(top, stamp)
    img = emit(top, order, total)
    if limit and total > limit:
        raise SystemExit("mkxiprom: %d bytes, more than %d" % (total, limit))
    with open(out, "wb") as fh:
        fh.write(img)
    with open(os.path.join(root, "etc/xiprom.list"), "w") as fh:
        fh.write("".join(f + "\n" for f in files))
    with open(os.path.join(root, "etc/xiprom.stamp"), "wb") as fh:
        fh.write(stamp)
    print("mkxiprom: %d files, %d bytes -> %s" % (len(files), total, out))


if __name__ == "__main__":
    main()
