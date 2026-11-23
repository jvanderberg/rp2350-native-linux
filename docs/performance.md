# Performance

Measured on the board, with both cores at 348 MHz, the flash and the PSRAM
at 116 MHz and the SD card's bus at 43.5 MHz.

## Summary

| | |
|---|---|
| CoreMark, gcc -O3 | 886 |
| CoreMark, gcc -O2 | 850 |
| CoreMark, built on the board with tcc | 163 |
| Syscall | 0.84 µs |
| `fork` + `exit` + `wait`, small ATRANS process | 5 ms |
| `vfork` + `exec` | 8.3 ms |
| Context switch between two ATRANS processes | about 180 µs |
| PSRAM read | 41 MB/s |
| PSRAM write | 13 MB/s |
| CRNG seeded (from the hardware TRNG) | 0.6 s after the kernel starts |
| ssh reachable | about 25 s after power-on |

## ATRANS processes

| | |
|---|---|
| `fork` + `exit` + `wait`, small process | about 5 ms |
| ...with 1 MiB of heap in use | about 60 ms (the copy runs at about 19 MB/s) |
| Growing the heap by 256 KiB in place | about 20 ms, nearly all of it zeroing the new heap |
| Growing with a copy (512 KiB to 1 MiB) | about 71 ms |
| Switch: cache flush and ATRANS write | about 130 µs |
| Pipe round trip (two switches) | about 0.35 ms, against 0.10 ms between FDPIC processes |

Most of a switch is refilling the 16 KiB XIP cache, which the switch empties
and which also holds the kernel's data and the kernel code that isn't in
SRAM ([qmi-atrans.md](qmi-atrans.md)).

## Where code runs

The XIP cache is the bottleneck for code: at a tight shell loop it hits
about 95% of the time, and the misses are about half the run time. What
runs from SRAM doesn't miss ([memory.md](memory.md)).

### The kernel's hot code

Pipes, fork and exec, ttys, the network and the SD card spend most of their
time in the kernel, and the kernel runs from flash: with all of it there, a
pipe round trip between FDPIC processes is 93% kernel time, spread over
hundreds of functions, most of it waiting on flash. `CONFIG_RP2350_HOT_TEXT`
builds the kernel with `-ffunction-sections` and links its hottest 128 KiB
into SRAM at 0x20000000: C functions one by one, and whole the exception
entry code, `.sched.text`, `.noinstr.text`, `memcpy` and `memset`. The
next 48 KiB goes first in flash `.text`, side by side. 128 KiB covers 93% of
the kernel's samples in the 13 workloads profiled.

With the flash at 116 MHz, the same root filesystem, three runs each:

| Kernel code in SRAM | none | 20 KiB | 64 KiB | 128 KiB |
|---|---|---|---|---|
| Pipe round trip, FDPIC | 443–492 µs | 116–165 µs | 108–128 µs | 96–109 µs |
| Pipe round trip, ATRANS | 1034–1056 µs | 494–495 µs | 364–369 µs | 353–355 µs |
| 30 × `echo x \| cat` | 0.58–0.60 s | 0.50 s | 0.43–0.44 s | 0.41–0.42 s |
| 2000-iteration `ash` loop | 0.82 s | 0.80–0.81 s | 0.78–0.79 s | 0.77–0.78 s |

With 128 KiB against none, 1.5 MB through ssh takes 3.1 s instead of 3.7 s to
send and 4.5–5.3 s instead of 5.3–5.7 s to receive, and 8 MiB read from the
SD card 2.7 s instead of 3.1 s. The `ash` loop is user code, which runs from
PSRAM.

How it is put together:

- `linux/mach-rp2350/sram-text.lds.inc` lists the input sections for
  SRAM, `hot-text.lds.inc` those that start flash `.text`, and
  `sram-marks.lds.inc` points the special sections' start and end symbols
  at their SRAM copies.
- `.sram.text` is linked ahead of `.text`, so `.text`'s wildcards don't take
  its sections, and is stored in flash right after `.head.text`, outside
  the compressed data. `__mmap_switched` copies it to SRAM before anything
  calls it.
- Calls between SRAM and flash are out of Thumb branch range: the linker
  adds a 16-byte veneer for each target.
- `core_kernel_text()` counts `.sram.text` as kernel text. The unwind table
  holds its entries first, then flash's: two sorted runs, which `unwind.c`
  searches separately.
- The sram-text MTD starts after it (`build.sh`).

To choose the lists again, profile a kernel with all its code in flash
(`CONFIG_RP2350_HOT_TEXT` off, `CONFIG_RP2350_PCSAMPLE=y`):

```sh
echo 150 > /proc/pcsample          # a sample every 150 µs or so (NMI)
WORKLOAD
echo 0 > /proc/pcsample; cat /proc/pcsample > NAME.bin
```

and run `tools/hot-text.py vmlinux System.map linux/mach-rp2350 128 48
*.bin` with that kernel's `vmlinux` and `System.map`. Every workload weighs
the same. A function that is renamed or gone just stops matching.

### busybox

Shell benchmarks for the FDPIC busybox, in seconds:

| busybox's text | uClibc | 2000-iteration `ash` loop | 30 × `echo x \| cat` | `md5sum` 512 KiB | `seq 3000 \| sed \| grep` |
|---|---|---|---|---|---|
| SRAM | flash, in place (the default) | 0.51 | 2.45 | 0.12 | 0.50 |
| SRAM | PSRAM (page cache) | 0.61–0.69 | 2.42 | 0.11 | 0.38 |
| flash | flash | 1.34 | 2.69 | 0.14 | 0.67 |
| PSRAM | PSRAM | 1.22 | 2.49 | | 0.50 |

uClibc from flash costs little except in the `sed` pipeline, and saves its
copy in RAM. Through the uncached alias the romfs is far slower (11 s for
the `echo` pipelines), so it is read through the cached one.

Linking the kernel's `.data` and `.bss` into SRAM (`SRAM_DATA=1`) gains
about 3%, within run-to-run noise: kernel data is not where the misses are.

## SD card

32 MiB with `dd bs=64k` after `drop_caches`, writes with `conv=fsync`:

| | Time | Rate |
|---|---|---|
| raw write, `/dev/mmcblk0p2` | 13.7 s | 2.45 MB/s |
| raw read, `/dev/mmcblk0p2` | 11.7 s | 2.86 MB/s |
| file write, FAT | 15.7 s | 2.14 MB/s |
| file read, FAT | 11.9 s | 2.81 MB/s |
| ATRANS swap-out / swap-in | | 3.18 / 3.13 MB/s |

The bus's ceiling at 43.5 MHz is 5.4 MB/s. The rest of a block's time is
core 1's work per block (waiting for the card's data token, the commands)
and, for reads, Linux copying the slots out; with one request in flight
those barely overlap. About one read block per request waits for a free
slot (`slot_waits`).

## Flash

The kernel's text runs from flash, so most XIP cache misses in the kernel
are flash reads. The boot stub runs the flash's QSPI clock at 116 MHz
(CLKDIV 3), the PSRAM's clock on the same bus. At 348 MHz, every RXDELAY
from 2 to 7 reads the flash correctly and 1 does not; the stub uses 4.
Against 87 MHz (CLKDIV 4), the same kernel, three runs each:

| | 87 MHz | 116 MHz |
|---|---|---|
| Pipe round trip, FDPIC | 534–556 µs | 443–492 µs |
| Pipe round trip, ATRANS | 1248–1263 µs | 1034–1056 µs |
| 30 × `echo x \| cat` | 0.65 s | 0.58–0.60 s |
| 2000-iteration `ash` loop | 0.84 s | 0.82 s |

The shell loop barely changes: its text is the ATRANS busybox's, in PSRAM.
Reads still send the 0xEB command with every access (no continuous-read
mode).

A 4 KiB erase of `/home` stalls the whole system for about 45 ms, since
both cores are parked while the boot ROM runs.

## Pins

From the two-board pin tests ([pin-tests.md](pin-tests.md)):

- PIO square waves within 0.001% at 1 kHz, 100 kHz and 1 MHz.
- GPIO edge events through the GPIO v2 uAPI: every edge at 5 ms spacing;
  PIO edge capture down to 5 µs apart.
- PIO DMA echo at about 18 M words a second.
