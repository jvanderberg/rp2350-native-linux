#!/usr/bin/env python3
"""check-atrans.py FILE...: checks ELF files against the ATRANS v2/v3 contract.

  - static ELF32 ET_EXEC EM_ARM, EABI5, no PT_INTERP/PT_DYNAMIC
  - every PT_LOAD inside the data window [0x11000000, 0x11400000) or (v3)
    the text window [0x11c00000, 0x12000000); text-window segments are
    read-only with p_filesz == p_memsz, writable ones are in the data window
  - the stack, [0x11000020, lowest data-window segment), is at least 8 KiB
  - code is Thumb only (no $a mapping symbols), and the disassembly for
    ARMv7E-M holds no VFP/NEON or coprocessor instruction (cp15 included), no
    undefined encoding and no kuser helper address (0xffff0fxx)

Needs unstripped files (the $t/$a/$d mapping symbols tell code from data) and
$OBJDUMP / $READELF (default atrans-objdump / atrans-readelf). Non-ELF files
are skipped. Prints one line per file; exit status 1 if any file fails.
"""
import os
import re
import struct
import subprocess
import sys

LO, HI = 0x11000000, 0x11400000        # data window (ATRANS4)
TLO, THI = 0x11C00000, 0x12000000      # text window (ATRANS7), v3
MIN_STACK = 8192
OBJDUMP = os.environ.get("OBJDUMP", "atrans-objdump")
READELF = os.environ.get("READELF", "atrans-readelf")

# mnemonics that must not appear: VFP/NEON (v*, legacy f*), coprocessor ops
BAD = re.compile(
    r"^(v[a-z]|fld|fst|fmrx|fmxr|fmrs|fmsr|fmdrr|fmrrd|fcpy|fadd|fsub|fmul|fdiv|fcmp|fsitod|ftosi|"
    r"mrc|mcr|mrrc|mcrr|ldc|stc|cdp|wldr|wstr|tmcr|tmrc)")
UNDEF = re.compile(r"UNDEFINED|undefined|\.inst|<illegal|UNPREDICTABLE|\(bad\)")
KUSER = re.compile(r"0xffff0f[0-9a-f]{2}\b")


def elf_info(path):
    with open(path, "rb") as f:
        h = f.read(52)
        if len(h) < 52 or h[:4] != b"\x7fELF":
            return None
        if h[4] != 1 or h[5] != 1:
            return {"error": "not ELF32 little-endian"}
        (e_type, e_machine, _, e_entry, e_phoff, _, e_flags, _, e_phentsize,
         e_phnum) = struct.unpack_from("<HHIIIIIHHH", h, 16)
        f.seek(e_phoff)
        phs = [struct.unpack("<IIIIIIII", f.read(e_phentsize)[:32]) for _ in range(e_phnum)]
    return {"type": e_type, "machine": e_machine, "entry": e_entry, "flags": e_flags, "phs": phs}


def check(path):
    info = elf_info(path)
    if info is None:
        return None
    errs = []
    if "error" in info:
        return [info["error"]], ""
    if info["type"] != 2:
        errs.append("not ET_EXEC (type %d)" % info["type"])
    if info["machine"] != 40:
        errs.append("not EM_ARM")
    if info["flags"] >> 24 != 5:
        errs.append("not EABI5 (e_flags %#x)" % info["flags"])
    if info["flags"] & 0x400:
        errs.append("hard-float ABI")
    loads = []   # (start, end, filesz, writable)
    for (p_type, _, p_vaddr, _, p_filesz, p_memsz, p_flags, _) in info["phs"]:
        if p_type == 3:
            errs.append("has PT_INTERP")
        elif p_type == 2:
            errs.append("has PT_DYNAMIC")
        elif p_type == 1 and p_memsz:
            loads.append((p_vaddr, p_vaddr + p_memsz, p_filesz, bool(p_flags & 2)))
    if not loads:
        errs.append("no PT_LOAD")
        return errs, ""
    data = [l for l in loads if LO <= l[0] and l[1] <= HI]
    text = [l for l in loads if TLO <= l[0] and l[1] <= THI]
    for a, b, fs, w in loads:
        if (a, b, fs, w) not in data and (a, b, fs, w) not in text:
            errs.append("segment %#x-%#x outside the windows" % (a, b))
    for a, b, fs, w in text:
        if w:
            errs.append("writable segment %#x-%#x in the text window" % (a, b))
        if fs != b - a:
            errs.append("segment %#x-%#x in the text window has bss" % (a, b))
    if not (info["entry"] & 1):
        errs.append("entry %#x is not Thumb" % info["entry"])
    stack = (min(a for a, _, _, _ in data) if data else LO + 0x10000) - LO - 0x20
    if stack + 0x20 < MIN_STACK:
        errs.append("stack %d bytes, under %d" % (stack, MIN_STACK))
    if text:
        tsize = max(b for _, b, _, _ in text) - TLO
        dsize = (max(b for _, b, _, _ in data) - min(a for a, _, _, _ in data)) if data else 0
        summary = "v3: text %d KiB shared, data+bss %d KiB, stack %d KiB" % (
            (tsize + 1023) // 1024, (dsize + 1023) // 1024, (stack + 0x20) // 1024)
    else:
        lo = min(a for a, _, _, _ in loads)
        hi = max(b for _, b, _, _ in loads)
        filesz = sum(s for _, _, s, _ in loads)
        summary = "v2: image %#x-%#x (%d KiB, %d KiB in file), stack %d KiB" % (
            lo, hi, (hi - lo + 1023) // 1024, (filesz + 1023) // 1024, (lo - LO) // 1024)

    syms = subprocess.run([READELF, "-sW", path], capture_output=True, text=True).stdout
    mapping = re.findall(r"\s\$([atd])(?:\.\S*)?$", syms, re.M)
    if not mapping:
        errs.append("stripped: no mapping symbols, code not checked")
    elif "a" in mapping:
        errs.append("ARM-mode code ($a mapping symbols)")
    attrs = subprocess.run([READELF, "-A", path], capture_output=True, text=True).stdout
    if re.search(r"Tag_FP_arch: (?!No)", attrs) or re.search(r"Tag_Advanced_SIMD_arch: (?!No)", attrs):
        errs.append("FP/SIMD build attributes")
    cpu = re.search(r"Tag_CPU_arch: (\S+)", attrs)
    # gcc says v7E-M; LLVM (Rust) says v7 with the Microcontroller profile
    m_profile = re.search(r"Tag_CPU_arch_profile: Microcontroller", attrs)
    if cpu and cpu.group(1) not in ("v7E-M", "v7-M", "v6-M", "v6S-M") and \
            not (cpu.group(1) == "v7" and m_profile):
        errs.append("Tag_CPU_arch %s" % cpu.group(1))

    bad = []
    if mapping:
        dis = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", path],
                             capture_output=True, text=True).stdout
        func = "?"
        for line in dis.splitlines():
            m = re.match(r"^[0-9a-f]+ <(.*)>:$", line)
            if m:
                func = m.group(1)
                continue
            parts = line.split("\t")
            if len(parts) < 2 or not re.match(r"^\s*[0-9a-f]+:$", parts[0]):
                continue
            insn = "\t".join(parts[1:]).strip()
            mnem = insn.split()[0] if insn.split() else ""
            if insn.startswith(".word") or insn.startswith(".short") or insn.startswith(".byte"):
                if KUSER.search(insn):
                    bad.append("%s: kuser address %s" % (func, insn))
                continue
            if BAD.match(mnem) or UNDEF.search(insn):
                bad.append("%s: %s" % (func, insn))
    if bad:
        errs.append("%d bad instructions, e.g. %s" % (len(bad), "; ".join(bad[:3])))
    return errs, summary


def main():
    status = 0
    for path in sys.argv[1:]:
        if os.path.islink(path) or not os.path.isfile(path):
            continue
        r = check(path)
        if r is None:
            continue
        errs, summary = r
        if errs:
            status = 1
            print("FAIL %s: %s" % (path, "; ".join(errs)))
        else:
            print("ok   %s: %s" % (path, summary))
    return status


if __name__ == "__main__":
    sys.exit(main())
