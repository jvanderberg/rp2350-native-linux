# QMI ATRANS as base-and-bounds translation

The RP2350's QMI translates the XIP address space in eight 4 MiB windows,
`ATRANS0`–`7`. ATRANS0–3 cover flash (CS0) and ATRANS4–7 cover the PSRAM
(CS1, from 0x11000000). Each register has a BASE, in 4 KiB pages of the
device, and a SIZE, in 4 KiB pages; an access at or past SIZE is an error.
That is base and bounds: enough to give each process its own memory at
fixed addresses ([atrans.md](atrans.md)).

This file records how the hardware behaves, as far as the kernel relies on
it. The behaviour was established with a bare-metal test at 348 MHz, the
PSRAM (8 MiB APS6404L on QMI CS1) at 116 MHz, interrupts off.

## How this system uses the windows

| Window | Address | Use |
|---|---|---|
| ATRANS4 | 0x11000000 | the current ATRANS process's data block |
| ATRANS5, ATRANS6 | 0x11400000 | the kernel's RAM: PSRAM 0–8 MiB, set up by the boot stub |
| ATRANS7 | 0x11c00000 | the current process's shared text block |

The kernel's RAM starts at 0x11400000 rather than 0x11000000 so that the
first window is free for processes.

## Translation and bounds

- **Remap.** An access to window offset `off` reaches device address
  `(BASE × 4 KiB + off) mod 16 MiB`, through both the cached and the
  uncached alias, for reads and writes. The granularity really is 4 KiB.
  The 24-bit device address wraps at 16 MiB, and the 8 MiB chip ignores
  A23, so BASE 0x800 aliases BASE 0.
- **Bounds.** A load or store at or past `SIZE` × 4 KiB, cached or uncached,
  is a precise bus error, and execution can resume after it. A cached store
  past SIZE leaves no dirty line behind (the write-allocate fill fails), and
  the device word is unchanged. SIZE 0 allows nothing. The QMI is not
  wedged afterwards.
- **Instruction fetch** follows the window too, and is cached under the
  window address. A fetch past SIZE is an instruction access fault. Hot
  code runs from the cached window at about two-thirds of SRAM speed; the
  uncached alias is far too slow for code.
- **DMA** through 0x11000000 and 0x15000000 is translated as well, and a DMA
  read past SIZE sets the channel's `READ_ERROR`. DMA therefore follows
  whichever process is current: a driver doing DMA to a user buffer would
  have to use the untranslated alias, or finish before the switch.
- **ATRANS0–3** are independent: changing ATRANS4 doesn't affect flash.
- **At reset** ATRANS0–7 hold the identity map with SIZE 4 MiB:
  0x04000000, 0x04000400, 0x04000800, 0x04000c00, then the same four again.

## Aliases

| Address | Behaviour |
|---|---|
| 0x11000000 | cached, translated |
| 0x15000000 | uncached, translated |
| 0x19000000 | cache maintenance; loads fault (write-only operations) |
| 0x1d000000 | uncached, **untranslated**: the device address directly |
| 0x12…, 0x13…, 0x17…, 0x1f… (bit 25 set) | bus error |

0x1d000000 is the one untranslated alias, and the kernel uses it to reach a
block that isn't in the window (fork, swap, remote reads). The other
translated windows (0x11400000–0x11ffffff) reach other PSRAM. User mode is
fenced off from all of them by the MPU.

## The cache

The XIP cache is 16 KiB, shared by both cores, and serves flash and PSRAM
alike.

- **Virtually tagged.** After a BASE change with no flush, the cached alias
  still returns the old block's data.
- **Write-back, write-allocate.** A cached write reaches the device only
  when the line is cleaned or evicted. A write miss allocates the line and
  keeps the line's other word.
- **Write-back uses the translation at the time of the write-back.** A line
  dirtied under BASE A and cleaned after switching to B lands in **B**. An
  invalidate without a clean drops dirty data. So a switch must **clean
  under the old ATRANS, then write ATRANS, then invalidate**. The invalidate
  may come before the ATRANS write, as long as nothing touches the window in
  between.
- **Clean by address** (maintenance op 3) works as documented.

### Set/way clean rewrites the tag

A clean by set/way (op 1) at maintenance offset `o` writes a dirty line back
correctly, but leaves it valid with the tag of the maintenance address. The
line then aliases flash 0x10000000 + `o`, and a later flash fetch returns
PSRAM data. This is erratum RP2350-E11; the pico-sdk's
`xip_cache_clean_all` works around it the same way as here.

The workaround is to issue set/way cleans at the top of the 64 MiB
maintenance window, 0x18000000 + 0x03ffc000 + `o` (`XIP_SETWAY_HI` in
`rp2350-atrans-internal.h`):

- The retagged line then claims 0x13ffc000 + `o`, a bus-error region that
  nothing can hit.
- Dirty lines are written back and, in effect, dropped.
- **Clean lines are not touched**, so a switch still needs an invalidate
  pass. The kernel does both in one set/way loop.
- **Nothing may clean by set/way at low offsets.**

### Costs

Bare metal, maintenance code in SRAM, at 348 MHz:

| Operation | Time |
|---|---|
| Clean all, nothing dirty (2048 maintenance writes) | 35 µs |
| Invalidate all | 39 µs |
| Clean all, 16 KiB dirty | 317 µs (about 137 ns per dirty line) |
| One loop of clean + invalidate per set/way, then the ATRANS write | 56 / 149 / 240 / 343 µs with 0 / 4 / 8 / 16 KiB dirty |
| ATRANS write and read-back | 206 ns |
| Refill: a cold 8 KiB sweep | 182 µs (178 ns per 8-byte line); warm, 40 µs |

At most about 16 KiB is ever dirty (writing 32 KiB through the cache evicts
half of it first), so 343 µs is the worst case for the maintenance itself.
The invalidate also drops the kernel's data and whatever kernel code runs
from flash, so the real cost of a switch includes refilling both the
kernel's and the process's working sets: about 180 µs in all under Linux,
with the kernel's hot code in SRAM ([atrans.md](atrans.md)).

## Copy bandwidth

Copying PSRAM to PSRAM, 256 KiB:

| Method | Rate |
|---|---|
| CPU, uncached to uncached (0x1d or 0x15) | 9.5 MB/s |
| CPU, cached to cached, then clean | 6.8 MB/s (the write-allocate fills the destination first) |
| CPU, cached source to uncached destination (0x11 to 0x1d) | **13.8 MB/s** |
| DMA, uncached to uncached | 11.4 MB/s |
| DMA, cached source to uncached destination | 14.1 MB/s |

The kernel copies from the cached window to the uncached untranslated alias
(fork, growth by moving). The rates are near what QSPI allows with 4- and
8-byte transactions, each carrying about 14 clocks of command, address and
dummy overhead. Longer bursts (direct mode, the XIP stream FIFO) are not
used.

## The bootrom's flash routines

`connect_internal_flash`, `flash_exit_xip`, `flash_range_erase`,
`flash_range_program`, `flash_flush_cache` and `flash_enter_cmd_xip` leave
ATRANS0–7 alone. But `flash_exit_xip` overwrites CS1's read setup, and M0's
as well:

| Register | Before | After |
|---|---|---|
| `M1_TIMING` | 0x61d67403 | 0x6000720c |
| `M1_RFMT` | 0x000612aa | 0x00001000 |
| `M1_RCMD` | 0xeb | 0x03 |

`M1_WFMT` and `M1_WCMD` are kept. So every flash write must save and restore
M0, M1, the pads and `XIP_CTRL` around the bootrom calls, and clean dirty
PSRAM lines first, because `flash_flush_cache` invalidates the whole XIP
cache. The boot stub's flash routine (`boot/src/flash.rs`) does this.

## Constraints that follow

- **One translation and one cache for both cores.** Two cores can't run two
  different processes from the window at once, and a switch on one core
  flushes the other core's cache contents too. Linux runs on core 0 only.
- **The cache is small and shared.** 16 KiB holds the kernel's flash text,
  the kernel's data and the process. Every switch empties it.
- **Copying costs milliseconds.** At 14–19 MB/s, fork and growth by moving
  cost time in proportion to the memory in use.
- **Each process is one physical chunk.** Base and bounds can't scatter a
  process, so fragmentation has to be managed by the allocator: the ATRANS
  pool compacts, and swap makes room ([memory.md](memory.md),
  [atrans-swap.md](atrans-swap.md)).
