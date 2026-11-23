# Memory

The board has 8 MiB of PSRAM, 520 KiB of SRAM and 16 MiB of flash. Linux
gets the PSRAM as its RAM, runs its own text in place from flash, and
uses the SRAM for the code that runs most.

## Where things live

### PSRAM (8 MiB)

The boot stub maps PSRAM 0–8 MiB at 0x11400000 through ATRANS5 and ATRANS6,
and that is the kernel's RAM (`memory@11400000` in `rp2350.dts`). ATRANS4
and ATRANS7 stay free for the current process's windows
([atrans.md](atrans.md)). Everything is in this RAM: kernel data, slab, page
cache, tmpfs, FDPIC programs' data, and the ATRANS pool.

### SRAM

| Address | Contents |
|---|---|
| 0x20000000–0x2003BFFF | The kernel's hot code (`.sram.text`, 236 KiB with its veneers), copied from flash at boot |
| 0x2003C000–0x20067FFF | `sram-text` (mtd0, 176 KiB) |
| 0x20068000–0x2006BFFF | The I/O area shared with core 1 (`rp2350-io.h`): control blocks, the two network rings, the SD read slots, the flash page buffer and stack |
| 0x2006C000–0x2007FFFF | The boot stub's RAM (80 KiB): its data, core 1's SRAM code, core 1's stack, core 0's stack before Linux |
| 0x20080000–0x20081FFF | SRAM8/9: the console rings |

### Flash

The flash map is in the [README](../README.md#flash-map). The kernel runs
in place (XIP) from 0x10D80000; the root filesystem is a squashfs; the
shared libraries and larger read-only binaries run in place from the xip
romfs at 0x10200000.

## Code placement

Kernel text, the shared libraries and every program's code are fetched
through the same 16 KiB XIP cache as all of PSRAM. At a tight shell loop the
cache hits about 95% of the time, and the misses are about half of the run
time. So what runs most goes where it doesn't miss:

- **The kernel's hot code in SRAM.** The 224 KiB of kernel code that runs
  most (`CONFIG_RP2350_HOT_TEXT`): pipes, the scheduler, the exception
  entry, `memcpy`. A pipe round trip takes a third of the time it takes
  with the kernel all in flash ([performance.md](performance.md)).
- **The rest of SRAM: `sram-text`.** Before init starts, `overlay-init`
  copies the FDPIC busybox, cut after its last `PT_LOAD`, into the
  `sram-text` MTD as a one-file romfs (`tests/sramload.c`), mounts it at
  `/sram` and binds `/sram/busybox` over `/bin/busybox`. romfs on a RAM MTD
  supports direct mapping on nommu, so ELF-FDPIC maps busybox's text in
  place from SRAM, shared by every process. With the kernel's code there,
  the 176 KiB left is too small for busybox (388 KiB), so it runs in place
  from the xip romfs instead: it is only PID 1, `getty`, `telnetd` and
  `udhcpc` now, the shell being `busybox-atrans`.
- **Libraries in place from flash.** `mkxiprom.py` builds a romfs of the
  files in `xiprom.list` (uClibc, `ld-uClibc`, the gpio/i2c/pio libraries,
  `micropython.bin`, a fallback busybox), 64-byte aligned, ELF files cut
  after their last `PT_LOAD`. `overlay-init` mounts it at `/xip` and binds
  each file over its path, if the romfs's stamp matches the root
  filesystem's `/etc/xiprom.stamp`; otherwise the squashfs copies are used.
  It is read through the cached alias: the uncached one is several times
  slower. Text mapped in place costs no RAM.
- **Core 1's hot code** (the SD path, the USB stack, its loop, `mem*`) runs
  from SRAM, with the feature `core1-sram` (the default). The Wi-Fi driver
  stays in flash, except its bus transfers.
- **Kernel data in SRAM** is an option, not the default:
  `SRAM_DATA=1 run-build.sh kernel` sets `CONFIG_RP2350_SRAM_DATA`, which
  links the kernel's `.data`, init data, `.bss` and init stack at 0x20000000
  and doesn't free init memory (SRAM has no `struct page`). It gained about
  3% with the kernel's code all in flash. It takes the SRAM the kernel's hot
  code would use, so it turns `CONFIG_RP2350_HOT_TEXT` off.

`/proc/xipcache` reads the XIP cache's hit and access counters.

## Anything that grows is ATRANS

An ATRANS process's memory is one block that can be moved and swapped.
FDPIC memory and kernel memory stay where they were put. So shells and
interactive programs are ATRANS: `/bin/sh` and the applets are
`busybox-atrans`, and every login session, telnet, SSH and tmux shell,
`sh -c` and init script runs as ATRANS. FDPIC is left to PID 1
(`overlay-init`, then `init`) and a few small, fixed daemons (`getty`,
`login`, `telnetd`, `udhcpc`).

## The ATRANS pool

`rp2350-atrans-pool.c` sets aside PSRAM at boot for ATRANS blocks, data and
shared text: 5 MiB by default (`atrans_pool=` on the command line; 0 for
none). Kernel and FDPIC allocations never land in it, so every page of the
pool is either free or part of an ATRANS block.

- **Compaction.** Blocks can move, since a process sees only its windows.
  When no gap fits a request but the pool has enough free space, the
  stretch between unmovable blocks that needs the least copying slides
  down, leaving one free run at its top. Unmovable: the current process's
  block and text (except while it grows or forks), a block whose mmap lock
  is held, and a block still being filled. Nothing sleeps while blocks
  move, and the kernel is uniprocessor and not preemptible, so no task runs
  in a block while it moves.
- **Growth in place.** A block that grows has the blocks around it slid
  away (and may move itself), so it needs only the growth free, not a
  second, bigger block.
- **Elastic.** The pool's free space at its top is lent to the page
  allocator when the kernel side runs short:
  - to a waiting allocation through a shrinker in direct reclaim, when free
    pages are below the high watermark;
  - to an FDPIC `mmap` copy that finds no free run (exactly the run it
    needs);
  - down to `atrans_pool_min=` (2 MiB by default), and only once the page
    cache (the file LRUs) is mostly gone. tmpfs pages are unevictable and
    don't count.

  It grows back into free pages past its end when a block needs them.
  kswapd lends nothing: space lent comes back only if nothing unmovable
  lands right after the pool, and lending to keep kswapd's watermarks would
  leave the pool at its minimum with megabytes free in pieces no block fits.
- **Swap when lending isn't enough.** When the pool has nothing to lend (no
  free space, or none it can gather at its top because a block can't move
  just then), the waiting allocation still comes first: the shrinker swaps
  out the ATRANS process asleep longest to make room
  ([atrans-swap.md](atrans-swap.md)).
- **The kernel's reserve.** Blocks, the pool growing and FDPIC copies take
  free pages only down to the high watermark: the pages between min and
  high are the kernel's, so the pool growing back for a swap-in can't take
  what the shrinker just lent the kernel. Only when swap can't make room
  either does a last try go down to the low watermark.
- **Growth before a second block.** A process that grows first has others
  swapped out to make room for just the growth, in place, before it asks
  for a second, bigger block (old and new at once).
- **Bounded waits.** Waiting for a block (exec, fork, growth) swaps others
  out for 3 s at most, then fails with ENOMEM.
- **Outside the pool.** When the pool can't satisfy a request, the page
  allocator takes it ([atrans.md](atrans.md), "Where blocks come from").

`/proc/atrans` lists the pool's blocks, compactions, moves, the longest
move, and what was lent and taken back. `/proc/atrans-swap` counts the
swap-outs made for the kernel side.

## Failing the newcomer

Memory pressure should fail the request that caused it, not kill a program
already running:

- An ATRANS block (exec, fork, `brk`, `mmap`) that can't be had after
  compaction, swap and cache drops is ENOMEM for that call. So are an FDPIC
  `mmap` copy, fork's kernel stack, and a tmpfs write.
- If the OOM killer runs anyway, it takes the process whose allocation
  failed (`vm.oom_kill_allocating_task=1`, set by `S02memlimits`; "Out of
  memory (oom_kill_allocating_task)" in the log). Stock 6.12 registers the
  OOM sysctls only with an MMU; `mm/oom_kill.c` is patched to register them
  here.
- It never takes `dropbear`, `telnetd`, `watchd` or the other daemons
  (`S99oomadj`: -1000, and -999 for the ones that start sessions or jobs),
  even when the daemon's own allocation is the one that failed (stock
  exempts only -1000). -999 is reset to 0 at exec, so what those daemons
  start doesn't inherit it (`mm/oom_kill.c`, `fs/exec.c`, patched).
- When it has to choose (the allocating task is one of those daemons, or a
  kernel thread), it takes the process with the most resident memory, as
  with an MMU: its RSS ([below](#what-free-top-and-ps-report)) plus
  `oom_score_adj`. Swap isn't counted, unlike stock: killing a process that
  is swapped out frees no RAM (`mm/oom_kill.c`, patched).
  `/proc/<pid>/oom_score` shows the ranking.

## Bounded tmpfs

`/tmp` is 512 KiB, `/run` and `/dev/shm` 256 KiB, the root overlay 1 MiB. A
big file fails with ENOSPC instead of waking the OOM killer. Without an MMU
there is no `CONFIG_SHMEM`, so tmpfs is ramfs, and ramfs ignores `size=`:
the kernel adds it (`linux/fs/ramfs/rp2350-size.inc`; `df` shows it), and a
ramfs page that can't be had fails the write with ENOMEM.

## Kernel memory

Kernel memory can't move or be swapped, so it is kept small:

- **No BSD ptys**: they made 3000 of the 5000 sysfs nodes.
- **Smaller bio and squashfs reserves.**
- **exec drops the page cache** of what it copied into the process (FDPIC
  and ATRANS).
- **A small printk ring** (`CONFIG_LOG_BUF_SHIFT=13`): the default takes
  about 530 KiB with its descriptor arrays.
- **TCP socket buffers** are capped at 64 KiB (`S02memlimits`).
- **ttys.** A line discipline keeps 1 KiB read and echo buffers instead of
  4 KiB (canonical lines up to 1023 characters): 4 KiB per tty instead of a
  16 KiB order-2 allocation, two per pty pair. And a tty holds at most
  16 KiB that nobody has read yet, against 640 KiB in stock Linux; a program
  writing to a pty whose reader is swapped out would otherwise fill kernel
  memory.
- **FDPIC `mmap` copies** (`do_mmap_private`) that find no free power-of-two
  block take an exact run, the pool's lent space, a cache drop or a
  swap-out (`rp2350_alloc_exact`).

Unreclaimable slab comes to about 1.2 MB.

`/proc/memstat` and `/proc/memmap` (`CONFIG_RP2350_MEMSTAT`,
`linux/mm/rp2350-memstat.inc`) show slab by cache, large kmallocs by
caller and page kinds, and one character per page of RAM.

## What free, top and ps report

The kernel's own figures count the ATRANS pool and swap, so `free`, `top`,
`htop`, `ps` and `pulse` show the truth unchanged.

- **Free memory.** The pool is taken from the page allocator, so its free
  space would count as used. `MemFree` in `/proc/meminfo` and `freeram` from
  `sysinfo()` are the free pages plus the pool's free space, and
  `MemAvailable` adds the pool's free space too (`mm/show_mem.c`, patched).
  Used memory (`MemTotal` minus `MemFree`, `Buffers` and `Cached`) is then
  the kernel, FDPIC programs and ATRANS blocks, each shared text once.
- **Swap.** `SwapTotal` and `SwapFree` (`sysinfo()`'s `totalswap` and
  `freeswap`) are ATRANS swap ([atrans-swap.md](atrans-swap.md)), through
  `si_swapinfo` (`include/linux/swap.h`, patched; `total_swap_pages` stays
  0, so reclaim never tries to swap LRU pages). `/proc/swaps` lists the
  device or file, and `/proc/vmstat`'s `pswpout` and `pswpin` count the pages
  written and read (`CONFIG_VM_EVENT_COUNTERS`).
- **The pool**, at the end of `/proc/meminfo`: `ATransPool` (its size now:
  it lends and grows back), `ATransPoolFree`, `ATransBlocks` (every ATRANS
  block, data and text, in the pool or not) and `ATransText` (the shared
  text blocks).
- **Per process.** Stock nommu keeps no RSS counters; here they are kept,
  and `/proc/<pid>/stat` (VSZ, RSS), `statm`, `status`
  (`VmSize`, `VmHWM`, `VmRSS`, `RssAnon`, `RssFile`, `VmData`, `VmExe`,
  `VmSwap`, in the MMU kernel's format: `fs/proc/rp2350-task.inc`),
  `getrusage()`'s `ru_maxrss` and the OOM killer read them:
  - **An ATRANS process:** its block is `RssAnon`, its shared text
    `RssFile`, and what swap holds of it `VmSwap` (`set_block()` in
    `rp2350-atrans.c`, and the swap code). Swapped out, it has `RssAnon` 0,
    so its RSS is its text, which stays resident; its VSZ is still its
    block's size plus its text.
  - **An FDPIC process:** each of its mappings whose pages are RAM, while it
    is mapped: a private writable one as `RssAnon`, a read-only or shared
    file mapping as `RssFile` (`mm/nommu.c`, patched). Text run in place
    from flash or SRAM (busybox, uClibc) costs no RAM and doesn't count.
  - Shared text is in the RSS of every process that shares it, as file
    pages are with an MMU, so adding up RSS counts it more than once; the
    totals above count it once.

## Kernel configuration notes

- `physmap` rounds each window down to a power of two, which suits
  GPIO-banked flash; `apply.sh` patches it to use the exact size when there
  are no address GPIOs, so the 416 KiB `sram-text` MTD isn't cut to 256 KiB.
- romfs needs `CONFIG_MISC_FILESYSTEMS`; without it the config merge drops
  `ROMFS_FS` silently.

## Limitations

- `AnonPages`, `Mapped` and `Committed_AS` in `/proc/meminfo` read 0:
  nommu maps no pages through page tables and does no overcommit
  accounting, which is what keeps them. `ATransBlocks` and `MmapCopy` have
  the user memory.
- An ATRANS process has at most 4 MiB of data, in one contiguous block.
- FDPIC memory and kernel memory fragment the RAM outside the pool, and
  nothing can move them.
