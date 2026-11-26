# Boot, core 1 and the I/O devices

Linux runs on core 0. Core 1 is an I/O processor: it runs the USB console,
the Wi-Fi radio and the SD card, and carries out flash writes. Both run
from one Rust binary, the boot stub (`boot/`, `nativeboot`), at the start
of flash.

## The boot stub

On core 0, the stub:

1. Reads the board config (below) and sets the clocks to its CPU speed,
   and the flash's QSPI clock to at most 116 MHz.
2. Sets up the console rings in SRAM8/9 (0x20080000), the I/O area
   (0x20068000) and the USB console, which core 0 polls until core 1 takes
   it over.
3. Opens the setup screen if it has to, or counts down for a key that
   opens it.
4. Brings up the PSRAM on QMI CS1, and maps up to 8 MiB of it at
   0x11400000 through ATRANS5 and ATRANS6.
5. Looks for an SD card, and loads the Wi-Fi chip's firmware.
6. Copies the device tree into the top 16 KiB of the PSRAM with the config
   applied, and starts core 1.
7. Starts the boot watchdog, clears `ACTLR.EXTEXCLALL` and jumps to the
   XIP kernel at 0x10D80000 with r2 pointing at that copy.

Two orderings matter:

- **The PSRAM is set up before core 1 starts.** PSRAM init puts the QMI into
  direct mode, and any instruction fetch from flash during that time fails.
- **`ACTLR.EXTEXCLALL` must be clear on core 0.** The HAL's
  `Multicore::spawn` sets it, which sends every LDREX/STREX to the bus. The
  XIP/PSRAM path has no exclusive monitor, so STREX to kernel RAM would
  always fail and the kernel would hang in its first cmpxchg loop.

## Setup and the board config

One image runs on any RP2350A or RP2350B board with PSRAM, an SD card and
a CYW43439 radio. What differs from board to board is in the board
config, set in the stub's setup screen on the USB console and kept in the
4 KiB flash sector at 0x103FF000 (`boot/src/config.rs`). Nothing else
writes it: Linux's flash routine refuses that sector.

| Setting | Values |
|---|---|
| CPU speed | one of the five below |
| PSRAM chip select | GP0, GP8, GP19 or GP47 (the pins QMI CS1 can be on), or found at boot |
| SD card | a block of four pins in the SPI order, MISO, CS, SCK, MOSI (GP0–3, GP4–7, ... GP44–47); the block decides SPI0 or SPI1 (GP8–11, SPI1, by default) |
| Wi-Fi radio | WL_ON, DIO, CS, CLK: any pins, with DIO and CLK both in GP0–31 or both in GP16–47 (PIO0 sees 32 at a time) |
| Wi-Fi network | name and WPA passphrase (none for an open network) |
| Setup countdown | 0–60 s, 10 by default |

The sector has space reserved for options to come; it is zero now and
covered by the CRC.

**When setup opens.** When the sector holds no valid config (a new board
starts in setup and waits for a terminal); after two boots in a row that
didn't reach Linux's init; when Linux asked for it (`reboot config`); when
no PSRAM answers; and on any key during the countdown. Otherwise the stub
waits the countdown, then boots. A sector `tools/wifi.sh` wrote (older
images: a network only) isn't a config, but it boots the defaults with that
network, as before, so a board without a terminal keeps booting; save in
setup to replace it.

**The screen.** Up and down choose a line, and left and right change its
value. Enter on CPU speed, PSRAM chip select or SD card pins opens a list
of the choices (up and down, Enter to take one; a choice whose pins
something else uses says so). On the Wi-Fi lines and the countdown, Enter
types a value. On an action, it runs it. Esc cancels, and Ctrl-L draws the
screen again. The actions find the PSRAM, test the SD
card (it reports the card's size; pins the radio or the PSRAM has are
refused) and the radio (it powers the chip and reads its bus test word,
0xFEEDBEAD), save and boot, boot the saved settings (dropping what was
changed on the screen), and restore the defaults. Saving checks the config first:
no pin used twice, the SD pins on one SPI block, a passphrase of 8 to 63
characters. A new CPU speed takes a reboot, which skips the countdown.

**Failed boots.** POWMAN's scratch registers survive the watchdog's chip
resets (`boot/src/boot.rs`). The stub counts each boot as it starts; Linux
zeroes the count when `/sbin/overlay-init` writes `/proc/rp2350/boot_ok`.
The watchdog, at its longest (about 16.7 s), runs from the stub's start,
fed while the countdown and setup wait, so a CPU speed that hangs the stub
itself resets the board too. Before it starts Linux the stub starts the
watchdog again if the device tree has `/chosen/rp2350,boot-watchdog` (and
stops it if not), and `linux/mach-rp2350/board.c` feeds it once a second
until `boot_ok`, then stops it. The kernel feeds it for two minutes at
most, so a boot that hangs or panics, or never gets to `boot_ok`, resets
the board and counts as a failed boot. At two, setup opens, with the clocks
at the stock 150 MHz in case the saved speed is what fails; saving (or
booting the saved settings) goes back to the saved speed.

**`reboot config`.** `/sbin/reboot` is a script that writes `config` to
`/proc/rp2350/next_boot` before it runs BusyBox's reboot; the next boot
opens setup. `reboot(2)` with `LINUX_REBOOT_CMD_RESTART2` and `"config"`
does the same.

**What Linux is told.** The stub copies the device tree from flash into
the top 16 KiB of the PSRAM, which the memory node then leaves out
(`boot/src/fdt.rs`), and changes in it: every `clock-frequency` to the CPU
speed; the memory size to the PSRAM's (at most 8 MiB); `gpio-reserved-ranges`
and `gpio-line-names` to core 1's pins (`PSRAM_CS`, `SD_MISO`, ..., `WL_CLK`)
and those of the Linux peripherals that stay on; and `status = "disabled"`
on i2c0, i2c1, spi0 or the ADC when core 1 has one of their pins, or SPI0
for the SD card. On an RP2350A, GP30–47 are reserved and the ADC is off.
The boot banner lists what was turned off.

### CPU speed

| Setting | Core voltage | Flash and PSRAM clock |
|---|---|---|
| 150 MHz (stock) | 1.10 V | 75 MHz |
| 200 MHz | 1.10 V | 100 MHz |
| 250 MHz | 1.15 V | 83 MHz |
| 300 MHz | 1.15 V | 100 MHz |
| 348 MHz | 1.20 V | 116 MHz |

The flash and the PSRAM run at clk_sys over the smallest divider that
gives at most 116 MHz; the flash's read delay is at least 5.7 ns at each
speed (`board.rs`). Each setting was tested on the board, set and saved
through the setup screen: Linux booted to login with the SD card, swap
and Wi-Fi up, and:

| Setting | BogoMIPS | md5sum of 10 MB from /dev/zero | 10 MB read from the SD card |
|---|---|---|---|
| 150 MHz | 99.7 | 5.18 s | 4.77 s |
| 200 MHz | 133.0 | 3.96 s | 3.53 s |
| 250 MHz | 166.3 | 4.20 s | 4.13 s |
| 300 MHz | 199.1 | 3.54 s | 3.39 s |
| 348 MHz | 231.0 | 3.09 s | 3.32 s |

Most of what the system does waits on the flash and the PSRAM, so 250 MHz
(at 83 MHz on the QSPI bus) is slower than 200 MHz (at 100 MHz).

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
SRAM instead of through the XIP cache: the SD path, the USB stack, the loop,
`mem*` and the radio's poll (with the cyw43 driver's runner inlined into
it), 28 KB (`core1.x`, copied by `main`). The rest of the Wi-Fi driver
(bring-up and joins, about 10 KB of code) stays in flash.

That SRAM is shared with `.data`, `.bss` (core 1's 24 KiB stack among it)
and core 0's stack, which runs from the top of RAM down to the end of
`.bss` with nothing to stop it. Core 0 builds the radio's driver future
on it, about 6.3 KiB. `memory.x` fails the link if less than 8 KiB is
left. When core 0 overruns it, it overwrites `USB_BUS` and core 1 panics
in usb-device ("alloc_ep failed: EndpointOverflow", in the RTT buffer)
with no console. A change that makes rustc inline more into
`core1_loop` can grow `.core1_text` by kilobytes, so check its size.
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

- It joins the network in the board config (set in setup), and rejoins
  when the link goes down.
- Ethernet frames go to and from `rp2350-net.c` (`eth0`) through two rings
  of four slots in the I/O area.
- **Receive backpressure.** The cyw43 driver reads every frame the chip
  has waiting in one pass, but its channel holds four and it drops the
  rest. Access points send bursts of more than four frames, so TCP into
  the board lost frames on every window and backed off: about 500 KiB/s,
  with a hundred out-of-order segments per 2 MB. `PioSpi` now hides "F2
  packet available" in the status word while the channel is full
  (`rx_gate`), and the chip keeps the frames until there is room. If Linux
  leaves its ring full for 200 ms (eth0 down), core 1 throws frames away
  instead, so the chip's events and ioctl replies still get through. The
  driver's `receive` needs a free transmit slot too, so while the transmit
  side is full core 1 can't see the channel; then the gate stays open, as
  the transmit side drains only on the credit that comes with what is
  read. `/sys/class/net/eth0/radio_stats` counts data frames read, polls
  that left a frame in the chip, and frames thrown away.
- **Transmit** is scatter-gather with checksum offload, both done by
  `rpnet_xmit` on core 0. It gathers the frame's pieces into its ring slot
  (SRAM) and sums the TCP/UDP checksum over that copy. Without these
  features the stack's software GSO copied every segment out of the
  socket's buffer again, PSRAM to PSRAM, with its checksum: a fifth of core
  0's time when sending at full rate. A byte sent is now copied twice on
  core 0, from the user into the socket's buffer (PSRAM) and into the ring
  slot (SRAM).
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
  (0x10FF0000–0x10FFFFFF). The board config sector is written only by
  the stub's setup screen, before Linux runs.
- A 4 KiB erase stalls the whole system for about 45 ms. The `stats` file
  in the flash device's sysfs directory has the longest stall.

## Startup

The kernel mounts the squashfs (`root=/dev/mtdblock1`) and runs
`/sbin/overlay-init`, a script for the FDPIC busybox's hush, which:

1. Mounts the xip romfs at `/xip`, checks that its stamp matches the root
   filesystem's `/etc/xiprom.stamp`, and binds each file listed in
   `/etc/xiprom.list` over its path on the squashfs. With a stale or
   missing romfs the files in the squashfs are used.
2. Puts a 1 MiB tmpfs overlay on top of the squashfs, so the system is
   writable until reboot, and binds the romfs files there too.
3. Copies busybox into `sram-text` (`sramload`), mounts it at `/sram` and
   binds it over `/bin/busybox`, if it fits ([memory.md](memory.md)).
4. Mounts `/home`, but only if the flash region already holds jffs2 (it
   looks for a node's magic). It never formats or erases it; without jffs2,
   `/home` is in RAM.
5. Binds `/home/root` over `/root`.
6. Writes `/proc/rp2350/boot_ok` (the boot got this far: see "Failed
   boots" above).
7. Pivots into the overlay and starts busybox init. The squashfs, with the
   romfs files bound over it, stays visible at `/rom`.

What makes a boot slow is loading text out of the squashfs: an exec of a
program there copies (and decompresses) its text, and exec drops it from
the page cache again. So `overlay-init` runs its commands with the FDPIC
busybox rather than the applet links, which are `busybox-atrans` (470 KiB
of text per exec, since nothing keeps it loaded yet), and with the
romfs's busybox once it is mounted. Binding uClibc first on the squashfs
root makes every later command run in place from flash. It has no
`$(...)`, which hush on nommu runs in a shell re-executed from the
squashfs. The whole script takes about 2 s.

Each step writes a marker to the kernel log (`dmesg | grep overlay-init`),
and so does each init script (`dmesg | grep rcS:`). Init starts about
2.6 s after the kernel, the last init script about 5 s, and ssh is up
about 7 s after a reset.

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
| `S45mdnsd` | `mdnsd` (`tests/mdnsd.c`): answers as `rp2350.local` (the hostname) over mDNS and advertises ssh and telnet for service browsers |
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
