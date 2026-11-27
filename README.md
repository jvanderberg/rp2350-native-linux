# Native Linux on the RP2350

The Raspberry Pi RP2350 can boot Linux 6.12, accept an SSH connection over
Wi-Fi, and compile and run a C program in that session. The kernel runs
directly on one Cortex-M33 core, executing in place from flash with 8 MiB
of external PSRAM. The second core handles the USB console, Wi-Fi, SD card
and flash writes.

**The port supports `fork()` on a chip with no MMU.** On each context
switch, the kernel uses the RP2350 memory interface’s address translation
(ATRANS) to map the next process’s PSRAM into a fixed address window.
A forked child sees its own copy of the parent’s data at the same
addresses, so its pointers still work. The same mechanism lets Linux swap
an idle process to SD and restore it to a different spot in RAM.

## What runs on the board

- **A networked shell.** SSH to `root@rp2350.local`, start a tmux session,
  edit files with nano or mg, and inspect processes with htop. Dropbear
  supports X11 forwarding to a host X server.
- **A native C compiler.** TinyCC ships as `cc`. You can write a program,
  compile it and run it from the board’s shell. The SD card holds larger
  projects, and a 1 MiB flash filesystem keeps your home directory across
  reboots and firmware updates.
- **Hardware control from Linux.** Drive GPIO, I²C, SPI, PWM and ADC through
  Linux device interfaces. Load PIO programs from C with libpio or from
  MicroPython with the `machine` and `rp2` modules. [Pin interfaces and examples](pins/README.md)
- **Applications with source.** The optional SD card bundle includes a file
  manager, an X terminal, a web server, `zip` and `unzip`, and games including
  Wolfenstein 3D.
  [Extras catalogue](extras/README.md)

## Hardware

Check the memory and peripheral wiring before flashing. Setup lets you
select the CPU clock (150–348 MHz) and assign pins to match your board.

| Component | Requirement or default wiring |
|---|---|
| MCU and flash | RP2350A or RP2350B with 16 MiB of flash |
| External RAM | 8 MiB APS6404L PSRAM on QMI CS1. Chip select can be GP0, GP8, GP19 or GP47. |
| Wi-Fi | CYW43439 (RM2) on PIO0. Defaults follow the Pico 2 W wiring, with WL_ON on GP23, data/IRQ on GP24, CS on GP25 and clock on GP29. |
| microSD | SPI0 or SPI1. The default is SPI1 with MISO on GP8, CS on GP9, SCK on GP10 and MOSI on GP11. |
| USB | Connection to a host for flashing and the serial console |

Core 1 owns the configured peripheral pins, PIO0 and the SD card’s SPI
controller. The remaining pins are available to Linux. [Board setup and I/O](docs/boot-and-io.md)

## Installing

1. Download `rp2350-native-linux-VERSION.uf2` from the [latest release](https://github.com/jvanderberg/rp2350-native-linux/releases/latest).
   Hold BOOTSEL while connecting USB and copy the UF2 to the `RP2350`
   drive. The copy takes a few minutes and the board restarts.
2. Open the USB serial console with the command for your host.

   ```sh
   screen /dev/cu.usbmodemnative1 115200   # macOS
   screen /dev/ttyACM0 115200              # Linux
   ```

3. A fresh board opens setup. Press Ctrl-L to redraw a blank screen.
   Check the pins, use **Find the PSRAM** if you need to identify its chip
   select, and enter your Wi-Fi credentials. Choose **Save and boot**,
   then log in on the console as `root` with an empty password.
4. On first install, initialize persistent storage for `/home`, which
   includes `/root`. Until then, home directories live in RAM. Run the
   first command below and confirm that `mtd3` is named `"home"` before
   running the erase command. It destroys any contents of that partition.

   ```sh
   grep home /proc/mtd
   flash_eraseall -j /dev/mtd3
   reboot
   ```

For firmware updates, repeat step 1. The UF2 preserves both the home
partition and the saved board settings. To revisit setup, run
`reboot config` or press a key during the boot countdown.

### Connect and compile

From the console, put your SSH public key in `/root/.ssh/authorized_keys`.
Dropbear accepts public keys only and keeps its host keys in
`/home/dropbear`. Connect from your host with `ssh root@rp2350.local`.
If your host lacks mDNS, use the DHCP address shown by
`ip addr show eth0` on the console. Telnet on port 23 also gives a root
shell without authentication or encryption. Use it only on a trusted network.

Try the native compiler from either shell:

```sh
cd /root
echo 'int main(void) { return 42; }' > answer.c
cc -o answer answer.c
./answer
echo $?    # 42
```

### Use the SD card

Linux mounts the first FAT partition at `/mnt/sd`. To install the extras,
unpack `rp2350-extras-VERSION.zip` on your host and copy the `extras`
directory to the card. Try `/mnt/sd/extras/games/asteroids` from the shell.
The release includes `SHA256SUMS` for verifying the downloads.

For swap, use a second partition with a Linux swap signature or a swap file
at `/mnt/sd/atrans.swp`. [Swap setup with `atrans-mkswap`](docs/atrans-swap.md)

## Process model and limits

The shell, compiler and interactive tools are static ATRANS binaries.
Each process gets a contiguous data block in PSRAM and the Cortex-M33’s
memory protection unit confines its access. Processes running the same
v3 binary share its read-only code through a second window. PID 1 and a
few small daemons use the conventional Linux no-MMU FDPIC format.

Each ATRANS data window is limited to 4 MiB. Fork copies the process’s
data, and switching windows flushes the 16 KiB XIP cache. Linux uses one
core and lacks `mprotect`, writable `MAP_SHARED`, `dlopen` and `ptrace`.

Measurements on the board at **348 MHz** show how those costs scale:

| Operation | Measured time |
|---|---|
| SSH reachable after reset | About 7 s |
| `fork` + `exit` + `wait`, small process | About 5 ms |
| Same operation with 1 MiB of heap in use | About 60 ms |
| Context switch between ATRANS processes | About 180 µs |

The design notes cover the [process ABI and fork](docs/atrans.md),
[QMI address translation](docs/qmi-atrans.md), [memory allocation](docs/memory.md)
and [benchmark conditions and results](docs/performance.md).

## Building from scratch

Install Git, Python 3, Rust via rustup, and Docker with about 25 GB free
in its disk. Install picotool to load a local build. The host-side loading
scripts assume macOS. A clean build takes about an hour on a four-core
arm64 GitHub runner. Subsequent builds are incremental.

```sh
./build-all.sh                # Build userland, toolchains, kernel and boot stub
./load.sh                     # Load the images over USB
```

Hold BOOTSEL when connecting the board for the first load. On subsequent
loads the script can reset the running board over USB. It checks image
sizes, skips unchanged filesystem images and preserves the board config
and home partition. Use `./load.sh --stub-only` to update just the boot stub.

Buildroot uses the revision pinned in `run-build.sh` and builds in the
Docker volume `rp2350-native-work`. Images appear in `out/`. Use
`./run-build.sh kernel` for a kernel-only rebuild or `./run-build.sh rootfs`
after changing a staged userland tree. Run `tools/release.sh VERSION` to
package the UF2 and extras in `out/release/`.

See the [build sequence](build-all.sh), [ATRANS toolchain](atrans/README.md),
[TinyCC patches and scripts](tcc/) and [two-board pin tests](docs/pin-tests.md).

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

For a large program, compile each source with `cc -c`, then set
`TCC_LINK_SCRATCH=/mnt/sd` when linking the objects with `cc`. The bundled
TinyCC uses the SD card for the merged code and relocation tables during
the static link. The Vim extra sets this variable for its own build.

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
