#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""mkuf2.py OUT.uf2 STUB.elf FILE@ADDR...: one UF2 for drag-and-drop onto
the RP2350's BOOTSEL drive, holding what load.sh writes: the boot stub (an
ELF, loaded at its segments' physical addresses) and the binary files at
their flash addresses (xip.romfs, the DTB, the root file system, the
kernel).

The boot ROM erases and writes only the 4 KiB sectors the blocks are in,
so /home (0x10100000-0x101FFFFF) and the board config sector (0x103FF000),
which this refuses to touch, keep what they hold.

As picotool's elf2uf2 does (picotool/elf2uf2/elf2uf2.cpp):
- family rp2350-arm-s, 256-byte pages, block_no 0..n-1 in address order;
- each sector a block lands in is filled with zero pages, as the boot ROM
  works out the sectors to erase from the block numbers;
- first, the RP2350-E10 block: an absolute-family block at 0x10ffff00
  that the boot ROM ignores (UF2_EXTENSION_RP2_IGNORE_BLOCK).
"""
import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
UF2_FLAG_EXTENSION_FLAGS_PRESENT = 0x00008000
UF2_EXTENSION_RP2_IGNORE_BLOCK = 0x9957E304
ABSOLUTE_FAMILY_ID = 0xE48BFF57
RP2350_ARM_S_FAMILY_ID = 0xE48BFF59
PAGE = 256
SECTOR = 4096
FLASH = (0x10000000, 0x11000000)
ABS_BLOCK_LOC = 0x11000000 - PAGE

# never written: /home's jffs2 and the board config (load.sh, flash map)
KEEP = [(0x10100000, 0x10200000, "/home"), (0x103FF000, 0x10400000, "the board config")]


def fail(msg):
    sys.exit(f"mkuf2: {msg}")


def elf_segments(path):
    """(physical address, bytes) of each loaded segment with file data."""
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        fail(f"{path}: not a 32-bit little-endian ELF")
    phoff, = struct.unpack_from("<I", d, 28)
    phentsize, phnum = struct.unpack_from("<HH", d, 42)
    segs = []
    for i in range(phnum):
        p_type, p_offset, _vaddr, p_paddr, p_filesz = struct.unpack_from("<IIIII", d, phoff + i * phentsize)
        if p_type == 1 and p_filesz:  # PT_LOAD
            segs.append((p_paddr, d[p_offset:p_offset + p_filesz]))
    return segs


def block(target, data, block_no, num_blocks, family, flags=UF2_FLAG_FAMILY_ID_PRESENT, payload=None):
    body = (payload if payload is not None else data.ljust(476, b"\0"))
    return struct.pack("<IIIIIIII", UF2_MAGIC_START0, UF2_MAGIC_START1, flags, target, PAGE,
                       block_no, num_blocks, family) + body + struct.pack("<I", UF2_MAGIC_END)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, stub, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    regions = [(a, b, stub) for a, b in elf_segments(stub)]
    for f in files:
        name, _, addr = f.rpartition("@")
        if not name:
            fail(f"{f}: want FILE@ADDR")
        regions.append((int(addr, 0), open(name, "rb").read(), name))

    pages = {}  # page address -> bytearray(PAGE)
    used = {}  # page address -> which of its bytes a region gave
    for addr, data, name in regions:
        end = addr + len(data)
        if not data:
            fail(f"{name}: empty")
        if not (FLASH[0] <= addr and end <= FLASH[1]):
            fail(f"{name}: {addr:#x}-{end:#x} is not all in flash")
        for lo, hi, what in KEEP:
            # the whole sectors it touches, as the boot ROM erases them
            if addr // SECTOR * SECTOR < hi and lo < -(-end // SECTOR) * SECTOR:
                fail(f"{name}: {addr:#x}-{end:#x} would erase {what} ({lo:#x}-{hi:#x})")
        off = 0
        while off < len(data):
            a = addr + off
            base = a - a % PAGE
            n = min(PAGE - a % PAGE, len(data) - off)
            pg = pages.setdefault(base, bytearray(PAGE))
            mask = used.setdefault(base, bytearray(PAGE))
            if any(mask[a - base:a - base + n]):
                fail(f"{name}: overlaps another region at {a:#x}")
            pg[a - base:a - base + n] = data[off:off + n]
            mask[a - base:a - base + n] = b"\1" * n
            off += n

    # whole sectors (zero pages), as picotool does
    for s in {p // SECTOR for p in list(pages)}:
        for p in range(s * SECTOR, (s + 1) * SECTOR, PAGE):
            pages.setdefault(p, bytearray(PAGE))

    order = sorted(pages)
    with open(out, "wb") as o:
        abs_payload = (b"\xef" * PAGE + struct.pack("<I", UF2_EXTENSION_RP2_IGNORE_BLOCK)).ljust(476, b"\0")
        o.write(block(ABS_BLOCK_LOC, b"", 0, 2, ABSOLUTE_FAMILY_ID,
                      UF2_FLAG_FAMILY_ID_PRESENT | UF2_FLAG_EXTENSION_FLAGS_PRESENT, abs_payload))
        for i, p in enumerate(order):
            o.write(block(p, bytes(pages[p]), i, len(order), RP2350_ARM_S_FAMILY_ID))
    sectors = sorted({p // SECTOR for p in order})
    print(f"mkuf2: {out}: {len(order)} pages in {len(sectors)} sectors, "
          f"{order[0]:#x}-{order[-1] + PAGE:#x}")


main()
