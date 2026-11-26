# Linux on the RP2350, natively

Linux 6.12 running natively on the Cortex-M33 of a Raspberry Pi RP2350, a
microcontroller with no MMU. Core 0 runs a nommu kernel, executing in place
from flash, with 8 MiB of PSRAM as its RAM. Core 1 is an I/O processor: it
runs the USB console, the Wi-Fi radio and the SD card, and carries out
flash writes.

Without an MMU, Linux normally offers only `vfork` and one shared address
space. Here the QMI's address translation (ATRANS) gives each process its
own window at a fixed address, so programs get real `fork`, the MPU confines
each one to its window, and a whole process can be written to the SD card
and brought back elsewhere in memory.

## What works

- **Processes with real `fork`**, through ATRANS windows: the shell, tmux,
  SSH sessions and the userland's programs are static ATRANS binaries with
  their own address space ([docs/atrans.md](docs/atrans.md)).
- **Swap to the SD card**: idle processes are swapped out whole, and come
  back when they wake ([docs/atrans-swap.md](docs/atrans-swap.md)).
- **Wi-Fi and SSH**: the CYW43439 radio as `eth0`, DHCP, and dropbear.
  Logins take about 1 s with the Mac's default ssh settings, so no special
  client configuration is needed.
- **mDNS**: the board answers as `rp2350.local` (`ssh root@rp2350.local`),
  and its ssh and telnet show up in service browsers (`dns-sd -B
  _ssh._tcp`).
- **Telnet** on port 23.
- **The SD card**: FAT at `/mnt/sd`, and its swap partition.
- **`/home` on flash** (jffs2, 1 MiB), kept across reboots and reloads.
- **tcc on the board**: compile and run C programs on the board itself.
- **GPIO, I2C, SPI, PWM, ADC and PIO**, through the standard Linux
  interfaces (`/dev/gpiochip0`, `/dev/i2c-N`, `/dev/spidev0.0`, sysfs PWM,
  IIO), plus `/dev/pio1` and `/dev/pio2` with libpio and `pioasm`
  ([pins/README.md](pins/README.md)).
- **MicroPython**, with `machine` and `rp2` modules for the pins and PIO.
- **tmux and friends**: tmux, htop, nano, mg, less, Lua and ncdu from
  Buildroot. More programs are in [extras/](extras/README.md), built on
  the board with tcc: `mc` (a Midnight Commander lookalike), `tm` (a small
  tmux work-alike), `xterm`, games and demos, X11 programs, a small web
  server and a telnet BBS.

## Hardware

- **An RP2350A or RP2350B**, at 150 to 348 MHz (set in setup).
- **8 MiB of PSRAM** (APS6404L) on QMI CS1: GP0, GP8, GP19 or GP47.
- **16 MiB of flash**: the flash map below reaches 0x10FFFFFF.
- **A CYW43439 Wi-Fi module** (RM2), run from PIO0; by default wired as on
  the Pico 2 W: WL_ON GP23, data/IRQ GP24, CS GP25, clock GP29.
- **A micro SD card** on SPI0 or SPI1; by default SPI1 on GP8 MISO, GP9
  CS, GP10 SCK, GP11 MOSI. Its first partition is FAT; a second partition
  with a Linux swap signature becomes swap.
- **USB** to a host, for the console and loading.

Which pins these are is the board config, set in the boot stub's setup
screen ([docs/boot-and-io.md](docs/boot-and-io.md), "Setup and the board
config"). Those pins, PIO0 and the SD card's SPI block belong to core 1;
the other pins are Linux's ([pins/README.md](pins/README.md)).

## Releases

Each release on GitHub has:

- `rp2350-native-linux-VERSION.uf2`: the boot stub, kernel, device tree
  and root file system. Hold BOOTSEL while plugging the board in, then
  copy it onto the `RP2350` drive (about 10 MB of data; the copy takes a
  few minutes). The board then boots; a new board starts in the setup
  screen on the USB console (below). The UF2 leaves `/home` and the board
  config as they are, so an update keeps both.
- `rp2350-extras-VERSION.zip`: [`extras/`](extras/README.md) with its
  programs built by the userland's gcc at `-O2`, to copy to the SD card
  (the board has no `unzip`: unzip it on the computer). The sources are
  there too, so each program can still be rebuilt on the board with `cc`.
- `SHA256SUMS`.

The workflow `.github/workflows/release.yml` builds them from scratch
(`./build-all.sh`, then `tools/release.sh`) on a GitHub arm64 runner when
a `v*` tag is pushed.

## Building from scratch

Needs Docker (with about 25 GB free in its disk), git, Python 3, Rust
(rustup; `rust-toolchain.toml` pins the release and the targets) and, to
load, picotool. The scripts assume a Mac host (`/dev/cu.*`, `stty -f`).
Everything but the boot stub and `claude-guest-mcp` builds in Docker.
Buildroot is cloned into `src/buildroot` at the release pinned in
`run-build.sh` (`BR_TAG`, `BR_REV`); build trees go in the Docker volume
`rp2350-native-work` (`WORK_VOLUME=name` picks another); outputs go in
`out/`. None of these are in git.

`./build-all.sh` runs the whole sequence. It takes about 2–3 hours the first
time, most of it the two Buildroot toolchains, and each step is
incremental after that:

1. `./run-build.sh rootfs`: the native Buildroot (gcc, uClibc-ng FDPIC,
   busybox). Its toolchain builds the kernel, its tree seeds the pins
   build, and it downloads the busybox source `busybox-atrans` is built
   from.
2. `atrans/run-build.sh all`: the ATRANS toolchain (gcc, musl) and packages,
   `claude-guest-mcp` (`claude-remote/`, host Rust),
   `busybox-atrans`; then `out/atrans-root/` and
   `out/atrans-sysroot.tar.gz` ([atrans/README.md](atrans/README.md)).
3. tcc ([tcc/](tcc/)): `tcc/apply-patches.sh` (tinycc from its GitHub
   mirror, or repo.or.cz, plus `tcc/patches/`), then in the tcc image (`tcc/run-host.sh sh
   /native/tcc/...`): `import-sysroot.sh /native/out/atrans-sysroot.tar.gz`,
   `host-build.sh` (the cross tcc), and with `BOARD_LIBC=/usr/lib/atrans`
   `native-build.sh /native/out/tcc-root` (the board's tree: `/usr/bin/tcc`,
   `/usr/lib/tcc`; the C library is the ATRANS sysroot's).
4. `pins/run-build.sh`: `out/pins-root/` ([pins/README.md](pins/README.md)).
   Its PIO tests are built with the ATRANS gcc (step 2) and tcc (step 3).
5. `./run-build.sh all`: the root filesystem again, now with
   `out/atrans-root/`, `out/tcc-root/` and `out/pins-root/` copied in
   (`post-build-devices.sh`), `out/xip.romfs`, and the kernel.
6. `cd boot && cargo build --release`: the boot stub (`load.sh` does this
   too).

Then `tools/release.sh [VERSION]` makes the release files in
`out/release/` (the UF2, with `tools/mkuf2.py`, and the extras zip, with
`tools/extras-gcc.sh`).

Partial rebuilds:

- `./run-build.sh kernel` rebuilds only the kernel, and `./run-build.sh
  rootfs` only the root filesystem (run it after changing any of the
  `out/*-root/` trees).
- `NATIVE_TAG=name ./run-build.sh` builds in trees of its own, for parallel
  work.
- `SRAM_DATA=1 ./run-build.sh kernel` links the kernel's `.data`/`.bss`
  into SRAM ([docs/memory.md](docs/memory.md)).

Tests that need no board:

```sh
atrans/test-qemu.sh                          # ATRANS programs under qemu-arm (Docker)
make -C br-external/package/libpio/test test # libpio and pioasm against a fake /dev/pio
python3 -m unittest discover -s br-external/package/micropython-machine/test
(cd boot/sdcard && cargo test --target "$(rustc -vV | sed -n 's/^host: //p')")
tcc/run-host.sh sh /native/tcc/host-test.sh  # and tests2.sh, tcctest.sh, extras.sh
```

The board's tcc under qemu-arm needs the C library beside it, as on the
board: copy `out/tcc-root` to a directory in the volume, unpack
`out/atrans-sysroot.tar.gz` into its `usr/lib/atrans`, and give that
directory to `native-test.sh DIR` and `ROOT=DIR selfhost.sh`.

## Loading

```sh
./load.sh             # stub + xipImage + DTB (+ rootfs.squashfs, xip.romfs if changed) over USB BOOTSEL
./load.sh --stub-only # only the boot stub
```

`load.sh` builds the boot stub, checks each image against its slot, and
loads them with picotool. It gets the board into BOOTSEL itself: through the
console's 1200-baud reset when this system is running, or through a debug
probe (probe-rs, `PROBE_RS=path`). The first time, hold BOOTSEL while
plugging the board in. The root filesystem and the xip romfs are written
only when they differ from what the board holds (`picotool verify`).

It never writes `/home` or the board config sector.
`STUB_CARGO_FLAGS=--no-default-features ./load.sh` builds the stub with all
of core 1's code in flash instead of SRAM.

### Flash map

| Address | Contents |
|---|---|
| 0x10000000 | Boot stub (`nativeboot`) |
| 0x10100000 | `/home`: jffs2, 1 MiB (mtd3). Kept across reboots; never formatted or erased by `load.sh` or at boot |
| 0x10200000 | xip romfs, at most 2016 KiB (mtd2): shared libraries and large read-only files, run in place and bound over the root filesystem at boot |
| 0x103F8000 | Device tree |
| 0x103FF000 | Board config (written only by the boot stub's setup screen; read-only to Linux) |
| 0x10400000 | Root filesystem: squashfs, at most 0x980000 bytes (mtd1) |
| 0x10D80000 | `xipImage`: kernel text and compressed data, at most 0x270000 bytes |
| 0x10FF0000 | Scratch, 64 KiB (mtd4): free to erase, for tests |

## Using it

**Console.** Over USB:

```sh
screen /dev/cu.usbmodemnative1 115200    # log in as root, no password
```

**Setup.** The boot stub has a setup screen on the console, where the
board config is set: the CPU speed, the PSRAM's chip select, the SD
card's and the Wi-Fi radio's pins, and the Wi-Fi network. A board with no
config starts in it; otherwise the stub counts down 10 seconds at each boot
(press a key for setup), or `reboot config` on the board opens it at the
next boot ([docs/boot-and-io.md](docs/boot-and-io.md)).

**Wi-Fi.** Set the network in setup. The board then joins at every boot,
rejoins when the link drops, and takes an address by DHCP. `ip addr show
eth0` on the console shows it.

**/home.** `/root` is `/home/root`, on flash. On a board whose `/home`
region doesn't hold jffs2 yet, `/home` is in RAM and nothing is formatted
for you. To create it, erase the region as jffs2 from the console, which
destroys whatever it held, and reboot:

```sh
grep home /proc/mtd             # the MTD named "home": mtd3
flash_eraseall -j /dev/mtd3
reboot
```

**SSH.** dropbear on port 22 accepts public keys only. Put your public key
in `/root/.ssh/authorized_keys` on the board (from the console), then
`ssh root@ADDRESS`. Host keys are made at the first connection and kept in
`/home/dropbear`.

X11 forwarding works: `ssh -Y root@ADDRESS PROGRAM` shows an X program
from the board on the host's X server (XQuartz on a Mac). The login gets
its own display, `DISPLAY=localhost:10.0` for the first (`:11.0` for a
second at the same time, and so on), and `/usr/bin/xauth` (`atrans/xauth.c`) stores the session's cookie
in `~/.Xauthority`, where the X client must find it and send it.

**Telnet.** `telnet ADDRESS`, root with no password. It is unencrypted and
unauthenticated, so use it only on a network you trust.

**SD card.** A card's first FAT partition is mounted at `/mnt/sd`, at boot
or when it goes in. A partition with a Linux swap signature (or a file
`/mnt/sd/atrans.swp`) becomes swap; `atrans-mkswap` makes one
([docs/atrans-swap.md](docs/atrans-swap.md)).

**Compiling on the board.** `tcc` (also `cc`) builds ATRANS programs:
`cc -o hello hello.c && ./hello`.

## How it works

- **Boot.** The boot stub sets up the clocks and PSRAM, maps the PSRAM at
  0x11400000 for the kernel, starts core 1, and jumps to the kernel in
  flash. `overlay-init` puts a tmpfs overlay on the read-only squashfs,
  binds the xip romfs's files in place, copies busybox's text into SRAM if it
  fits, and mounts `/home` ([docs/boot-and-io.md](docs/boot-and-io.md)).
- **Two kinds of program.** ATRANS programs are static binaries linked in
  the window at 0x11000000; each process's data is one PSRAM block, which
  the kernel maps there on every switch. A v3 binary's text sits in a
  second window, shared by every process running it. FDPIC programs are
  ordinary nommu programs; they run PID 1 and a few small daemons.
- **Memory.** Anything that grows runs as ATRANS, because an ATRANS block
  can be moved and swapped. A 5 MiB pool holds only ATRANS blocks and
  compacts them; its free space is lent to the kernel when the kernel is
  short ([docs/memory.md](docs/memory.md)).
- **I/O.** Linux talks to core 1 through rings and control blocks in a
  shared SRAM area and SIO doorbells. Core 1 never writes PSRAM, so the
  cache flushes at each process switch can't lose its data.

## Measured performance

On the board, at 348 MHz ([docs/performance.md](docs/performance.md)):

| | |
|---|---|
| CoreMark, gcc -O3 / -O2 | 886 / 850 |
| CoreMark, built with tcc on the board | 163 |
| Syscall | 0.84 µs |
| `fork` + `exit` + `wait` | 5 ms |
| `vfork` + `exec` | 8.3 ms |
| PSRAM read / write | 41 / 13 MB/s |
| CRNG seeded from the hardware TRNG | at 0.6 s |

## Known limitations

- **No MMU**, so no `mprotect`, no writable `MAP_SHARED` and no `dlopen`.
- **4 MiB of data per process**, the size of the ATRANS window, in one
  contiguous block.
- **Context switches cost:** about 180 µs between two ATRANS
  processes. A switch cleans and invalidates the 16 KiB XIP cache, and then
  both the kernel and the process refill their working sets: a pipe round
  trip takes about 0.35 ms, against 0.10 ms between FDPIC processes, which
  don't switch windows ([docs/performance.md](docs/performance.md)).
- **One CPU for Linux:** core 1 is the I/O processor, and the ATRANS windows
  and the XIP cache are shared by both cores.
- **No ptrace.**

## Repository layout

| Path | What it is |
|---|---|
| `build-all.sh`, `run-build.sh`, `build.sh` | The build: everything in order; the native Buildroot and the kernel (in Docker) |
| `load.sh` | Loads the images onto the board over USB BOOTSEL |
| `buildroot.defconfig`, `busybox.fragment`, `post-build.sh`, `post-build-devices.sh` | The native userland: Cortex-M4 code (ARMv7-M, which the M33 runs), soft-float, uClibc-ng, FDPIC, busybox; the root filesystem's scripts, services and the hook that copies in the `out/*-root/` trees |
| `patches/` | Fixes to Buildroot's packages for the native userland (`BR2_GLOBAL_PATCH_DIR`): uClibc-ng's own calls to `__aeabi_read_tp` bind locally, so FDPIC threads work, and its `daemon()` keeps the exiting parent off the child's stack; BusyBox's shells leave no history temp files when /home is full |
| `mkxiprom.py`, `xiprom.list` | The xip romfs: files run in place from flash |
| `linux.fragment`, `linux-devices.fragment`, `linux-pins.fragment`, `linux-sram-data.fragment` | Kernel configuration, merged on top of tinyconfig |
| `rp2350.dts`, `rp2350-pins.dtsi` | The device tree |
| `linux/` | Added to a stock 6.12 tree by `linux/apply.sh`: the machine (`mach-rp2350`), the drivers (`drivers/rp2350`: timer, console, network, SD, flash, TRNG, pins, PIO, ATRANS, the pool and swap), and patches to the core kernel |
| `boot/` | The boot stub and core 1's firmware (Rust); `boot/sdcard` is the SD card protocol, `boot/firmware` the radio's firmware |
| `atrans/` | The ATRANS toolchain (Buildroot, musl, patched gcc and binutils), its packages and the tree they make ([atrans/README.md](atrans/README.md)) |
| `tcc/` | TinyCC for ARMv7-M Thumb-2 ATRANS programs: patches, build and test scripts |
| `pins/`, `br-external/` | The pin tools' userland: libpio, MicroPython's `machine`, examples ([pins/README.md](pins/README.md)) |
| `extras/` | Programs for the board: `mc`, `tm`, `xterm`, games and demos, `httpd`, a BBS, Wolfenstein 3D, `ws2812` ([extras/README.md](extras/README.md)) |
| `claude-remote/` | `claude-remote` and `claude-guest-mcp`, an MCP tool server built as an ATRANS program |
| `tests/` | Kernel tests (`forktest`, `forktest3`, `swaptest`), the FDPIC switch baseline `pingpong`, `sramload`, `watchd`, and the ATRANS linker scripts |
| `docker/` | The image the Buildroot builds run in (`rp2350-native-build`) |
| `.github/workflows/` | `release.yml`: the whole build and the release files on a GitHub arm64 runner |
| `tools/` | `release.sh`, `mkuf2.py` and `extras-gcc.sh` (the release files), `preflight.sh` (sourced by the scripts), board session tools (`board-lock.sh`, `boot-wait.py`, `serial-chat.py`, `board-cmd.py`), `hot-text.py` (which kernel code goes in SRAM, from `/proc/pcsample` profiles), and the two-board pin harness (`pin-test.py`, `pintester/`) |
| `docs/` | Design notes and measurements (below) |

## Documentation

| Document | Subject |
|---|---|
| [docs/atrans.md](docs/atrans.md) | ATRANS processes: the ABI, blocks, fork, switching, the MPU, the kernel changes |
| [docs/atrans-swap.md](docs/atrans-swap.md) | Whole-process swap to the SD card |
| [docs/memory.md](docs/memory.md) | Memory layout, code placement, the ATRANS pool, OOM policy, kernel memory |
| [docs/boot-and-io.md](docs/boot-and-io.md) | The boot stub, core 1's console, Wi-Fi, SD card and flash writes, startup and init scripts |
| [docs/qmi-atrans.md](docs/qmi-atrans.md) | How the QMI's ATRANS translation and the XIP cache behave |
| [docs/performance.md](docs/performance.md) | Measurements |
| [docs/pin-tests.md](docs/pin-tests.md) | Testing the pins against a second board |

## License

The project's own code is MIT ([LICENSE](LICENSE)), with these exceptions:

- **`linux/`** is GPL-2.0-only, as Linux is: the drivers, the machine, and
  what `linux/apply.sh` adds to or changes in the kernel. The uapi header
  that user space includes (`linux/drivers/rp2350/rp2350-pio.h`) is
  `GPL-2.0-only WITH Linux-syscall-note`, so MIT programs such as libpio can
  include it. See [linux/COPYING](linux/COPYING). Each file's SPDX line
  says which applies.
- **Code, data and firmware from other projects** keep their own licenses.
  Each file says where it came from, with its copyright and an SPDX line,
  and the license texts are next to it:
  - `boot/`: the cyw43-pio port in `wifi.rs`
    ([boot/LICENSE-cyw43-pio](boot/LICENSE-cyw43-pio)), rp-hal's `memory.x`
    ([boot/LICENSE-rp-hal](boot/LICENSE-rp-hal)), the pico-sdk's
    `vreg_set_voltage` in `board.rs`
    ([boot/LICENSE-BSD-3-Clause](boot/LICENSE-BSD-3-Clause)). `board.rs`'s
    PSRAM bring-up follows FanPico's GPL-3.0-or-later `psram.c` closely;
    its header says what is unresolved.
  - `boot/firmware`: the CYW43439 firmware blobs, under the Infineon
    Permissive Binary License
    ([boot/firmware/README.md](boot/firmware/README.md)).
  - `tools/pintester/`: rp-hal's link scripts
    ([LICENSE-rp-hal](tools/pintester/LICENSE-rp-hal)), and the saved
    debugprobe firmware, which is Raspberry Pi's and others'
    ([its NOTICE](tools/pintester/debugprobe-v2.3.1-backup.NOTICE.txt)).
  - `br-external/package/libpio` and `micropython-machine`: pico-sdk,
    pico-examples and MicroPython parts (their `LICENSE*` files).
  - `extras/`: Wolf4SDL (GPL-2.0) in `wolf3d/`, and the pico-examples'
    `ws2812.pio` ([extras/README.md](extras/README.md), "Licenses").
  - `tcc/patches`: changes to TinyCC (LGPL-2.1-or-later), which is
    downloaded at build time.
