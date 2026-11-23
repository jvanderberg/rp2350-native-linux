# Boot, core 1 and the I/O devices

Linux runs on core 0. Core 1 is an I/O processor: it runs the USB console,
the Wi-Fi radio and the SD card, and carries out flash writes. Both run
from one Rust binary, the boot stub (`boot/`, `nativeboot`), at the start
of flash.

## The boot stub

On core 0, the stub:

1. Sets the clocks to 348 MHz, at 1.20 V, and the flash's QSPI clock to
   116 MHz.
2. Brings up the PSRAM on QMI CS1.
3. Maps PSRAM 0–8 MiB at 0x11400000 through ATRANS5 and ATRANS6.
4. Sets up the console rings in SRAM8/9 (0x20080000) and the I/O area
   (0x20068000).
5. Looks for an SD card, and loads the Wi-Fi chip's firmware.
6. Starts core 1.
7. Clears `ACTLR.EXTEXCLALL` and jumps to the XIP kernel at 0x10D80000 with
   r2 pointing at the DTB (0x103F8000).

Two orderings matter:

- **The PSRAM is set up before core 1 starts.** PSRAM init puts the QMI into
  direct mode, and any instruction fetch from flash during that time fails.
- **`ACTLR.EXTEXCLALL` must be clear on core 0.** The HAL's
  `Multicore::spawn` sets it, which sends every LDREX/STREX to the bus. The
  XIP/PSRAM path has no exclusive monitor, so STREX to kernel RAM would
  always fail and the kernel would hang in its first cmpxchg loop.

The M33 needs no CPU support patches in the kernel: upstream 6.12 already
identifies it (`__v7m_cm33_proc_info`). The kernel side of the machine is
`linux/mach-rp2350` and `linux/drivers/rp2350`, added to a stock 6.12 tree
by `linux/apply.sh`.

## Core 1

Core 1 loops over the USB console, the radio and the SD card. It sleeps in
`wfi` between passes and wakes on USB, on a doorbell from core 0 (SIO
doorbells, `SIO_IRQ_BELL`) or on a 1 ms tick. Core 0 gets doorbell IRQ 26
back when a frame has arrived or a request is done. The layout of the I/O
area they share is `linux/drivers/rp2350/rp2350-io.h`, mirrored in
`boot/src/io.rs`.

A PIO interrupt is not a wake source: the cyw43 driver leaves PIO0's IRQ
pending, which would keep core 1 spinning.

With the feature `core1-sram` (the default), core 1's hot code runs from
SRAM instead of through the XIP cache: the SD path, the USB stack, the loop
and `mem*`, 17 KB (`core1.x`, copied by `main`). The Wi-Fi driver (about
43 KB) stays in flash, except its bus transfers.
`STUB_CARGO_FLAGS=--no-default-features ./load.sh` builds the stub with all
of core 1's code in flash.

### Console

The console is a USB CDC-ACM port (`/dev/cu.usbmodemnative1` on a Mac), and
the kernel's tty, console and earlycon over the SRAM rings
(`rp2350-ringcon.c`, `ttyRC0`). Opening the port at 1200 baud and then
closing it reboots the board into BOOTSEL; `load.sh` uses this.

### Wi-Fi

`boot/src/wifi.rs` runs the CYW43439 with the `cyw43` driver, polled
without an executor. Its bus is a port of embassy's `cyw43-pio` to
rp235x-hal on PIO0, with the CPU feeding the PIO's FIFOs where the original
uses DMA.

- It joins the network stored in the Wi-Fi settings sector (0x103FF000,
  which it only reads; `tools/wifi.sh` writes it), and rejoins when the
  link goes down.
- Ethernet frames go to and from `rp2350-net.c` (`eth0`) through two rings
  of four slots in the I/O area.
- `/sys/class/net/eth0/joins` counts joins; writing to `.../rejoin` forces
  one.
- **Core 1 must keep up with the bus.** If polling falls behind, the RX FIFO
  fills and the `in` that completes a word stalls with the clock high,
  after the chip, in high-speed mode, has already moved on to the next bit.
  The word's last bit then comes back as the next word's first, and frames
  arrive corrupted. So `PioSpi::read` and `write` run from SRAM, where core
  0's use of the XIP cache can't slow them. The last two words of
  `/sys/class/net/eth0/core1_stats` count word reads that found the FIFO
  full (it should stay 0) and words read.

### SD card

`boot/src/sd.rs` runs the card on SPI1 at 43.5 MHz with the `sdcard` crate
(`boot/sdcard`: commands with CRCs, retries, insertion and removal). It
takes one request at a time from `rp2350-sd.c` (`mmcblk0`), up to 64 KiB in
up to 8 buffers, and checks for a card being inserted or removed about once
a second.

- **Writes** are sent straight from Linux's buffers in PSRAM, through the
  XIP cache both cores share, so no copy and no cache maintenance are
  needed. The CRC16 of each block is computed from each byte as it is sent
  (`SdBus::send_block`), not from memory before or after: a process woken
  during its own swap-out, or the page cache, can change the memory under
  the transfer, and a CRC over different bytes makes the card refuse the
  block.
- **Reads never write PSRAM.** At each switch between ATRANS processes,
  Linux cleans and invalidates the XIP cache by set/way, two operations per
  line, and a line core 1 wrote between them would be lost. So a read's
  blocks go to three 512-byte slots in the I/O area. Core 1 hands each one
  over with its destination and CRC16, and `rp2350-sd.c`'s interrupt copies
  it out (`memcpy_fromio`) and checks the copy against the CRC
  (`mismatch`). Core 1 fills a slot only once Linux has copied it, so the
  card waits with its clock stopped. Only core 0 dirties PSRAM lines, so the
  flush can't lose any. Core 1 still reads PSRAM to send writes; dropping a
  clean line loses nothing.
- **Clock.** The card takes high-speed mode (CMD6), so the driver asks for
  50 MHz. The PL022 divides clk_peri (= clk_sys, 348 MHz) by an even
  prescaler times an integer, so it runs at 43.5 MHz; the next step up,
  58 MHz, is over the card's limit. The bus's ceiling is then 5.4 MB/s.
- **Requests.** Sequential reads become multi-block commands (CMD18 +
  CMD12) of up to 64 KiB.
- **Busy time.** The card's busy time after a write is deferred: the next
  command waits it out. Between the blocks of a multi-block write it must be
  waited out in SPI mode.
- **CRC16** is folded into the transfer loops, with its table in SRAM.

`/sys/bus/platform/devices/20068000.sd/stats` has three lines:

1. requests, the time core 1 spends in them (`busy_us`), the bytes, the
   time spent moving data blocks on the bus (`xfer_us`), and `clock`, the
   rate the PL022 really runs at;
2. the error counters: CRC errors (`crc`, both ways), written blocks the card
   refused (`rejects`), waits that ran out (`timeouts`), requests that failed
   every try (`failed`), card initialisations, the longest busy wait, the
   last error (kind | byte << 8, sd.rs `err_code`), and requests refused for
   their buffers (`bad`);
3. the read slots: blocks that waited for a free slot (`slot_waits`), copies
   that didn't match the block's CRC (`mismatch`, which should stay 0), and
   slots that named no buffer of the request (`bad_dest`).

Throughput is in [performance.md](performance.md). DMA for SPI1 and two
requests in flight would go further; neither is built.

### Flash writes

Linux can't erase or program the flash it is executing from.
`rp2350-flash.c` calls a routine of the stub's in SRAM (`flash.rs`), with
interrupts off. That routine parks core 1 in SRAM, runs the boot ROM's
erase or program, restores the QMI's XIP setup (the ROM overwrites it:
[qmi-atrans.md](qmi-atrans.md)), and returns. While parked waiting on an SD
read slot, core 1 still answers the park request.

- It writes only `/home` (0x10100000–0x101FFFFF) and a scratch region
  (0x10FF0000–0x10FFFFFF). The Wi-Fi settings sector is never written by
  Linux.
- A 4 KiB erase stalls the whole system for about 45 ms. The `stats` file
  in the flash device's sysfs directory has the longest stall.

## Startup

The kernel mounts the squashfs (`root=/dev/mtdblock1`) and runs
`/sbin/overlay-init`, a script for the FDPIC busybox's hush, which:

1. Puts a 1 MiB tmpfs overlay on top of it, so the system is writable until
   reboot.
2. Mounts the xip romfs at `/xip`, checks that its stamp matches the root
   filesystem's `/etc/xiprom.stamp`, and binds each file listed in
   `/etc/xiprom.list` over its path. With a stale or missing romfs the
   files in the squashfs are used.
3. Copies busybox into `sram-text` (`sramload`), mounts it at `/sram` and
   binds it over `/bin/busybox` ([memory.md](memory.md)).
4. Mounts `/home`, but only if the flash region already holds jffs2 (it
   looks for a node's magic). It never formats or erases it; without jffs2,
   `/home` is in RAM.
5. Binds `/home/root` over `/root`.
6. Pivots into the overlay and starts busybox init. The squashfs stays
   visible at `/rom`.

Each step writes a marker to the kernel log (`dmesg | grep overlay-init`),
and so does each init script (`dmesg | grep rcS:`). Step 2, binding the
romfs files, takes about 8 s: ssh is up about 25 s after power-on.

The kernel starts init without `PATH`, so `overlay-init` sets it.

### Init scripts

| Script | What it does |
|---|---|
| `S02memlimits` | Caps TCP socket buffers at 64 KiB (kernel memory outside the pool); an OOM kill takes the process whose allocation failed |
| `S03sd` | Mounts the SD card's first FAT partition (or the whole card) at `/mnt/sd` if a card is in, and turns on swap on a swap partition. `S03sd event` (from watchd) handles insertion and removal |
| `S04atransswap` | Turns ATRANS swap on, and background swap-out: with less than 1.5 MiB free (pages plus the pool's free space), a process idle for 10 s goes to the card ([atrans-swap.md](atrans-swap.md)) |
| `S40network` | `eth0` with `udhcpc` in the background |
| `S41linkwatch` | `S41linkwatch event` (from watchd) renews the lease when the link comes back |
| `S42watchd` | `watchd` (`tests/watchd.c`): polls the card's size and `eth0`'s carrier every 2 s and runs the scripts above when they change |
| `S50dropbear` | SSH on port 22, if `dropbear` is in the image. Host keys go in `/home/dropbear` |
| `S50telnet` | `telnetd` on port 23 |
| `S99oomadj` | Keeps the OOM killer off the daemons (`oom_score_adj` -1000; -999 for those that start sessions or jobs) |

Without `fork`, some stock userland breaks, and the image works around it:

- `udhcpc -b` daemonizes by re-executing itself, which fails on nommu, so
  it runs with `-f` and the shell puts it in the background.
- Init scripts that need a background loop re-run themselves (`"$0" watch
  &`) or leave it to `watchd`, since hush can't background a shell
  function. (`/bin/sh` itself is the ATRANS ash, which has real `fork`.)

### Programs built elsewhere

The trees `out/atrans-root/` (ATRANS programs), `out/tcc-root/` (the C
compiler) and `out/pins-root/` (the pin tools) are copied over the root
filesystem by `post-build-devices.sh` when present. After changing them,
run `./run-build.sh rootfs` and then `./load.sh`. The files the previous
build copied are listed in `devices-hook.list` in the Buildroot output
directory and removed first. The copy uses `cp --remove-destination`, so a
file that replaces a busybox applet link (such as `less`) doesn't write
through the link onto `/bin/busybox`.

`post-build.sh` also copies `out/overlay/` (untracked) into the image, if
present, for local tests.

## The FAT's dirty flag

Linux keeps a FAT volume that was found dirty at mount marked dirty on disk,
however cleanly it is unmounted. So one reset without a shutdown (SWD,
BOOTSEL) makes every later mount say "Volume was not properly unmounted".
The board has no `fsck.fat`. Clearing bit 0 of byte 0x41 of the boot sector
(FAT32), with the volume unmounted, resets it. `reboot` leaves the volume
clean, even with a shell still in `/mnt/sd` (S03sd's lazy unmount completes
when init kills it).
