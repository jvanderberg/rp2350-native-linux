#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Choose the kernel's hot code from /proc/pcsample profiles and write the
linker script fragments that place it (CONFIG_RP2350_HOT_TEXT,
docs/performance.md "Kernel code placement"):

  sram-text.lds.inc    input sections linked into SRAM (.sram.text)
  sram-marks.lds.inc   the start/end symbols of special sections moved there
  hot-text.lds.inc     the next hottest functions, first in flash .text

usage: hot-text.py VMLINUX SYSTEM_MAP OUTDIR SRAM_KIB FLASH_KIB PROFILE...

VMLINUX and SYSTEM_MAP are those of the kernel the profiles were taken on
(VMLINUX may be a saved "arm-none-eabi-nm -S" listing of it instead).

Each profile is the raw /proc/pcsample output of one workload; every
workload weighs the same. Functions are taken by samples per byte. A C
function is its own section (-ffunction-sections: .text.NAME); the
special sections (.entry.text, .sched.text, ...) and assembly files move
whole. Assembly files not in ASM below are left where they are.
"""
import bisect
import collections
import os
import struct
import subprocess
import sys

# assembly files whose .text is one unit, by a symbol they define
ASM = {
    "arch/arm/lib/memcpy.o": ["mmiocpy", "memcpy", "__memcpy"],
    "arch/arm/lib/memset.o": ["mmioset", "memset", "__memset", "__memset32", "__memset64"],
    "arch/arm/lib/memmove.o": ["memmove", "__memmove"],
    "arch/arm/lib/memchr.o": ["memchr"],
    "arch/arm/lib/strchr.o": ["strchr"],
    "arch/arm/lib/strrchr.o": ["strrchr"],
    "arch/arm/lib/lib1funcs.o": ["__udivsi3", "__aeabi_uidiv", "__divsi3", "__aeabi_idiv",
                                 "__umodsi3", "__modsi3", "__aeabi_uidivmod", "__aeabi_idivmod"],
    "arch/arm/lib/div64.o": ["__do_div64"],
    "arch/arm/lib/getuser.o": ["__get_user_1", "__get_user_2", "__get_user_4", "__get_user_8"],
    "arch/arm/lib/putuser.o": ["__put_user_1", "__put_user_2", "__put_user_4", "__put_user_8"],
    "arch/arm/lib/findbit.o": ["_find_first_zero_bit_le", "_find_next_zero_bit_le",
                               "_find_first_bit_le", "_find_next_bit_le"],
    "arch/arm/lib/copy_page.o": ["copy_page"],
    "arch/arm/lib/csumpartial.o": ["csum_partial"],
    "arch/arm/lib/ashldi3.o": ["__ashldi3", "__aeabi_llsl"],
    "arch/arm/lib/lshrdi3.o": ["__lshrdi3", "__aeabi_llsr"],
    "arch/arm/lib/ashrdi3.o": ["__ashrdi3", "__aeabi_lasr"],
    "arch/arm/lib/muldi3.o": ["__muldi3", "__aeabi_lmul"],
    "arch/arm/lib/setbit.o": ["_set_bit"],
    "arch/arm/lib/clearbit.o": ["_clear_bit"],
    "arch/arm/lib/testsetbit.o": ["_test_and_set_bit"],
    "arch/arm/lib/testclearbit.o": ["_test_and_clear_bit"],
    "arch/arm/lib/csumpartialcopy.o": ["csum_partial_copy_nocheck"],
    "arch/arm/lib/call_with_stack.o": ["call_with_stack"],
    "arch/arm/mm/cache-nop.o": ["nop_flush_icache_all", "nop_flush_kern_cache_all",
                                "nop_flush_kern_cache_louis", "nop_flush_user_cache_all",
                                "nop_flush_user_cache_range", "nop_coherent_kern_range",
                                "nop_coherent_user_range", "nop_flush_kern_dcache_area",
                                "nop_dma_flush_range", "nop_dma_map_area", "nop_dma_unmap_area"],
    # IRQ and PendSV entry, the context switch (SVC entry is .entry.text)
    "arch/arm/kernel/entry-v7m.o": ["__invalid_entry", "__irq_entry", "__pendsv_entry",
                                    "__switch_to", "__fault_entry"],
}
# special sections that move whole: (section, start marker, end marker)
SPECIAL = [
    (".entry.text", "__entry_text_start", "__entry_text_end"),
    (".irqentry.text", "__irqentry_text_start", "__irqentry_text_end"),
    (".softirqentry.text", "__softirqentry_text_start", "__softirqentry_text_end"),
    (".noinstr.text", "__noinstr_text_start", "__noinstr_text_end"),
    (".sched.text", "__sched_text_start", "__sched_text_end"),
]
SKIP = ("cpu_v7m_", "cpu_cm7_")	# proc-v7m.S: runs before the copy, and idle


def rank(name, asm_of):
    """which of the names at one address its section is named after"""
    if name in asm_of:
        return 0
    if name.startswith("__se_sys_"):	# SYSCALL_DEFINE: sys_NAME is an alias
        return 1
    return 3 if name.startswith("sys_") else 2


def main():
    vmlinux, sysmap, outdir, sram_kib, flash_kib = sys.argv[1:6]
    profiles = sys.argv[6:]
    marks = {}
    for line in open(sysmap):
        a, _, n = line.split()[:3]
        marks[n] = int(a, 16)
    stext, etext = marks["_stext"], marks["_etext"]
    # noinstr.text holds cpuidle.text: only the part before it is noinstr's own
    # .cpuidle.text, idle, stays where it is; noinstr.text holds it (in a
    # kernel that has none of it in SRAM), so noinstr's own part ends there
    idle = (marks.get("__cpuidle_text_start", 0), marks.get("__cpuidle_text_end", 0))
    ranges = []
    for sec, s, e in SPECIAL:
        lo, hi = marks[s], marks[e]
        if sec == ".noinstr.text" and lo <= idle[0] <= hi:
            hi = idle[0]
        if hi > lo:
            ranges.append((lo, hi, sec, s, e))

    funcs = []
    with open(vmlinux, "rb") as f:
        elf = f.read(4) == b"\x7fELF"
    if elf:
        nm = subprocess.run(["arm-none-eabi-nm", "-S", vmlinux], capture_output=True,
                            text=True, check=True).stdout
    else:			# a saved "nm -S" listing of the profiled kernel
        nm = open(vmlinux).read()
    # code the profiled kernel already ran from SRAM counts too
    sram = (marks.get("__sram_text_start", 0), marks.get("__sram_text_end", 0))
    for line in nm.splitlines():
        f = line.split()
        if len(f) == 4 and f[2] in "tTwW":
            a, size = int(f[0], 16), int(f[1], 16)
            if (stext <= a < etext or sram[0] <= a < sram[1]) and size:
                funcs.append((a, size, f[3]))
    funcs = sorted(set(funcs))
    # *(.text.NAME) takes every function of that name: static helpers have
    # dozens of copies, so a name costs all of them
    name_size = collections.Counter()
    for a, size, name in funcs:
        name_size[name] += size
    asm_of = {s: o for o, syms in ASM.items() for s in syms}
    asm_size = collections.Counter()
    for a, size in {(f[0], f[1]) for f in funcs if f[2] in asm_of}:
        asm_size[asm_of[[f[2] for f in funcs if f[0] == a and f[2] in asm_of][0]]] += size
    # one name per address for the lookup: the one its section is named after
    best = {}
    for a, size, name in funcs:
        if a not in best or rank(name, asm_of) < rank(best[a][2], asm_of):
            best[a] = (a, size, name)
    lookup = sorted(best.values())
    addrs = [f[0] for f in lookup]

    def unit(pc):
        """(key, pattern, size, kind) of the code at pc, or None"""
        if idle[0] <= pc < idle[1]:
            return None
        for lo, hi, sec, s, e in ranges:
            if lo <= pc < hi:
                return (sec, sec, hi - lo, "special")
        i = bisect.bisect_right(addrs, pc) - 1
        if i < 0 or pc >= lookup[i][0] + lookup[i][1]:
            return None
        a, size, name = lookup[i]
        if name.startswith(SKIP):
            return None
        if name in asm_of:
            o = asm_of[name]
            return (o, o, asm_size[o], "asm")
        return (name, name, name_size[name], "c")

    score = collections.Counter()
    units = {}
    for p in profiles:
        d = open(p, "rb").read()
        n = len(d) // 8
        if not n:
            continue
        c = collections.Counter()
        for i in range(n):
            pc = struct.unpack_from("<I", d, 8 * i)[0]
            if pc & 1:
                continue			# user mode
            u = unit(pc)
            if u:
                c[u[0]] += 1
                units[u[0]] = u
        for k, v in c.items():
            score[k] += v / n / len(profiles)

    order = sorted(score, key=lambda k: -score[k] / units[k][2])
    total = sum(score.values())
    sram, flash = [], []
    sb = fb = 0
    for k in order:
        _, pat, size, kind = units[k]
        if sb + size <= int(sram_kib) * 1024:
            sram.append(k)
            sb += size
        elif kind == "c" and fb + size <= int(flash_kib) * 1024:
            flash.append(k)
            fb += size
    hdr = "/* generated by tools/hot-text.py from %s: do not edit */\n" % \
        " ".join(os.path.basename(p) for p in profiles)
    with open(os.path.join(outdir, "sram-text.lds.inc"), "w") as f:
        f.write(hdr)
        for k in sram:
            _, pat, size, kind = units[k]
            if kind == "special":
                s = [r for r in ranges if r[2] == pat][0]
                f.write("__sram%s = .; *(%s) __sram%s = .;\n" % (s[3], pat, s[4]))
            elif kind == "asm":
                f.write("*%s(.text)\n" % pat)
            else:
                f.write("*(.text.%s)\n" % pat)
    with open(os.path.join(outdir, "sram-marks.lds.inc"), "w") as f:
        f.write(hdr)
        for k in sram:
            if units[k][3] == "special":
                s = [r for r in ranges if r[2] == units[k][1]][0]
                f.write("%s = __sram%s; %s = __sram%s;\n" % (s[3], s[3], s[4], s[4]))
    with open(os.path.join(outdir, "hot-text.lds.inc"), "w") as f:
        f.write(hdr)
        for k in flash:
            f.write("*(.text.%s)\n" % k)
    print("SRAM: %d units, %d bytes, %.1f%% of kernel samples" %
          (len(sram), sb, 100 * sum(score[k] for k in sram) / total))
    print("flash first: %d functions, %d bytes, %.1f%% more" %
          (len(flash), fb, 100 * sum(score[k] for k in flash) / total))


main()
