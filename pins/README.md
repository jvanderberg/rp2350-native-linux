# The board's pins and PIO on native Linux

Native Linux on core 0 drives the RP2350's GPIO, I2C, SPI0, PWM, ADC, PIO1
and PIO2 itself, through the standard Linux interfaces (gpiochip, i2c-dev,
spidev, PWM sysfs, IIO) and `/dev/pioN`. The userland on top is libpio,
pioasm and MicroPython's `machine` and `rp2` modules (the libpio and
micropython-machine READMEs in `../br-external/package/`).

| Linux | Board | Driver (`linux/drivers/rp2350/`) |
|---|---|---|
| `/dev/gpiochip0`, lines 0-47 = GP0-GP47 | free pins GP1-GP3, GP12-GP15, GP17 (SPI0's chip select, held by spidev), GP20-GP22, GP26-GP28, GP30-GP39; pull-up/pull-down/none, rising/falling edge events | `rp2350-gpio.c` |
| `/dev/i2c-0`, `/dev/i2c-1` | I2C0 on GP4 (SDA) / GP5 (SCL), I2C1 on GP6 / GP7; 100 kHz, the pads' pull-ups | `rp2350-i2c.c` |
| `/dev/spidev0.0` | SPI0: GP16 RX, GP17 CS, GP18 SCK, GP19 TX; 100 kHz to 50 MHz, modes 0-3, 8-bit | `rp2350-spi.c` |
| `/sys/class/pwm/pwmchip0` (channel N = GPN) | any free GPIO below 40; pins on one slice share its period | `rp2350-pwm.c` |
| `/sys/bus/iio/devices/iio:device0` (`rp2350-adc`) | `in_voltage0..7` = GP40-GP47, `in_temp` | `rp2350-adc.c` |
| `/dev/pio1`, `/dev/pio2` | PIO1, PIO2 with DMA channels 12-15 | `rp2350-pio.c` |

GP0 (PSRAM), GP8-GP11 (SD card, SPI1), GP23/24/25/29 (Wi-Fi, PIO0) and USB
are core 1's: they are reserved in the device tree (`gpio-reserved-ranges`)
and no driver here touches them. PIO0 and SPI1 are core 1's too.

## Pieces

| Path | What |
|---|---|
| `../linux/drivers/rp2350/rp2350-{gpio,i2c,spi,pwm,adc,pio}.c`, `rp2350-pins.h` | the drivers, and what they share (RESETS, IO_BANK0, PADS, SIO; only through the atomic SET/CLR aliases, since core 1 changes the same registers for its pins) |
| `../linux/drivers/rp2350/rp2350-pio.h` | the PIO uapi, which libpio and the ATRANS sysroot include too |
| `../rp2350-pins.dtsi` | the device tree nodes, included at the end of `rp2350.dts` |
| `../linux-pins.fragment` | the kernel options, merged by `build.sh` after `linux.fragment` |
| `buildroot.fragment` | the userland packages, on top of `../buildroot.defconfig` |
| `run-build.sh`, `build.sh` | build the userland tree, `out/pins-root/` |
| `tests/` | `board-test.sh` (the checks below), `pinpeek.c` (a pin's registers, and frequency/duty measured from its pad) |
| `load-test.sh`, `pins-setup.sh`, `mkromfs.py` | testing only: the tree as a romfs in flash, mounted beside an initramfs root, for a kernel built with `PINS_TESTFS=1` (see the scripts) |

## Decisions

- **Own small drivers, not upstream i2c-designware and spi-pl022.** Both
  upstream drivers need the common clock framework (and spi-pl022 the AMBA
  bus), which this kernel doesn't have; i2c-designware also refuses
  zero-length messages, so `i2cdetect` would skip most addresses. The
  drivers here are small: I2C one byte in flight with every
  wait bounded (5 ms a byte, then a controller reset and bus recovery, and
  `-ETIMEDOUT`), a zero-length write sent as a one-byte read; SPI polled with
  the FIFO kept full. Clock rates come from the device tree
  (`clock-frequency`: the boot stub's 348 MHz clk_sys = clk_peri).
- **Chip select GP17 is a GPIO** (`cs-gpios`), so the line is valid in
  gpiolib and shows as used by `spi0 CS`.
- **GPIO edge interrupts** come from IO_BANK0's PROC0 interrupt registers on
  core 0's IO_IRQ_BANK0 (NVIC 21). Edges only, no level events.
- **PIO: direct register access.** A program maps a block's registers (and
  its DMA buffer) and drives them itself, at the chip's speed. Nothing
  filters its writes: CTRL's NEXTPREV bits and
  `irq prev`/`irq next` reach PIO0, the radio's block. A block drives only
  the pins whose FUNCSEL is that block's, which only the driver sets (on
  `RP2350_PIO_PIN_CLAIM`, through gpiolib).
- **DMA**: channels 12-15, DMA_IRQ_3,
  buffers in kernel memory (PSRAM through the XIP cache, which the DMA
  shares with the cores). A flash erase or program stops XIP, so the flash
  driver pauses the channels around it (`rp2350_pio_dma_pause/resume`,
  called by `rp2350-flash.c`).
- **mmap on nommu.** `/dev/pioN` maps directly (`get_unmapped_area` returns
  the registers' or the buffer's address, `mmap_capabilities` says
  `NOMMU_MAP_DIRECT`). That works for FDPIC processes, which run with the
  MPU off: libpio, `pioasm`, the PIO examples and MicroPython are FDPIC.
  ATRANS processes (the board's `cc`, musl): see "PIO from ATRANS
  programs" below. Everything else here (GPIO, I2C, SPI, PWM, ADC) is
  system calls and works from ATRANS programs as it is.
- **MicroPython is FDPIC** (Buildroot's unix port, with micropython-machine's
  `machine.mpy`, `rp2.mpy` and its `fcntl`/`_rp2350mem` C modules), because
  `rp2` maps `/dev/pio` (see above). `/usr/bin/micropython` is a wrapper that
  runs `micropython.bin -X heapsize=240K` (or `$MICROPY_HEAP`; 240K, not 256K, since nommu would round 256K plus a page to a 512 KiB block): the default
  1 MiB heap is one contiguous allocation, which nommu may not find.

## PIO from ATRANS programs

ATRANS processes run with the MPU confining user mode to their data window
(0x11000000-0x113FFFFF) and text window. `rp2350-atrans-dev.c` lets them
map `/dev/pioN` anyway:

- **mmap**: `MAP_SHARED` (read/write) of a character device that maps
  directly on nommu (`NOMMU_MAP_DIRECT` and a `get_unmapped_area`) returns
  the device's own address, outside the window: 0x50300000 / 0x50400000 for
  PIO1 / PIO2's registers, the buffer's kernel address (PSRAM, above
  0x11400000) for the DMA buffer. `PROT_EXEC` and `MAP_FIXED` are refused,
  ranges must be 32-byte aligned and miss both windows.
- **MPU**: each process has up to 3 such ranges (`ATRANS_NDEV`, in
  `struct rp2350_atrans_mm`). On switch-in (`mpu_confine`) they become MPU
  regions 5-7, read/write for user mode, execute-never; MMIO (at 0x40000000
  and up) with MAIR attribute 1, Device-nGnRE (set at boot), RAM with
  attribute 0, Normal write-through like the default map. A switch to a
  process with fewer ranges clears the rest; FDPIC processes run with the
  MPU off. Only changed regions are rewritten.
- **Region budget** (8 per security state): 0 data window, 1 thread-pointer
  granule (read-only), 2 text window (read-only, executable), 3-4 free
  (left to rp2350-atrans.c), 5-7 device ranges.
- **Lifetime**: a range holds a reference to its file (there is no VMA to
  do it), so `/dev/pio`'s release (block reset, buffer freed) waits for the
  last munmap. munmap of the range's start drops it (the whole range),
  exit and exec drop all, fork gives the child the same ranges (and
  references). A range mapped twice is counted.
- **Faults**: a load or store outside the windows and these ranges is a
  MemManage fault in user mode, which kills that process (SIGSEGV), as
  before. System calls still refuse pointers into a device range
  (`access_ok` covers the window only): `write(fd, pio_regs, n)` is
  `EFAULT`. `/proc/PID/maps` lists the ranges (`rw-s`, the device's path).

The userland side: `build.sh` builds libpio with the ATRANS musl toolchain
(`/work/atrans-br`, atrans) as a static library and installs it
into the board's ATRANS sysroot, where the board's `cc` (tcc, `../tcc/`)
looks:

| Path | Contents |
|---|---|
| `usr/lib/atrans/include` | `pio.h` (libpio), `rp2350-pio.h` (the uapi) |
| `usr/lib/atrans/lib` | `libpio.a` (libpio and pioasm, 29 KB) |
| `usr/share/examples/pio/atrans` | `squarewave pio-fifo pio-dma pio-edges ws2812`, ATRANS (musl gcc, v3) |
| `usr/libexec/pins-tests` | `atrans-pio` (ATRANS, see below); `tcc/` the same tests built by tcc |

so on the board `cc -o fifo /usr/share/examples/pio/pio-fifo.c -lpio`
builds an ATRANS PIO program. `build.sh` also builds `pio-fifo`,
`squarewave` and `atrans-pio` with `../tcc/`'s cross tcc (when its build
is in the volume, `/work/tcc-cross-board` and `/work/tcc-sysroot-board`,
and `out/tcc-root` exists) against that sysroot plus `libpio.a`, into
`usr/libexec/pins-tests/tcc/`, which is what the board's `cc` makes.

`atrans-pio [1|2]` (tests/atrans): a child touching the block's registers
before any mapping gets SIGSEGV; `pio_open` maps the registers at the
block's address and a state machine echoes 10000 words through the FIFOs;
the DMA buffer maps and DMA echoes 1024 words through it; `/proc/self/maps`
lists both; a forked child echoes through the inherited mapping and the
parent still can after it exits; after `pio_close` children touching the
registers or the buffer get SIGSEGV; the block opens again.

### Measured on the board

`atrans-pio` passes 12 of 12 (SIGSEGV without the mapping and after
munmap, with the kernel unaffected; fork), and so does the tcc build of it.
The ATRANS `squarewave` measures 1000.0 Hz; `pio-fifo` echoes 10000 words
with 0 wrong at about 2M round trips/s; `pio-dma` echoes 4096 words with 0
wrong. `atrans-pio 2` runs while an ATRANS `squarewave` holds PIO1: two
processes switching, each with its own block.

## The tree: `out/pins-root/`

`pins/run-build.sh` builds the packages in its own Buildroot
output (`/work/pins-br` in the work volume, seeded from the
native build's `/work/arm-br`), from `../buildroot.defconfig` plus
`buildroot.fragment`, so the shared libraries match the root filesystem's
uClibc. Rebuild it after `buildroot.defconfig` changes. It writes:

- `out/pins-root/`, copied over the root filesystem by
  `post-build-devices.sh`, as `atrans-root/` is;
- `out/pins-root.tar.gz` (owner root) and `pins-root.files`.

| Path | Contents |
|---|---|
| `usr/bin` | `gpiodetect gpioinfo gpioget gpioset gpiomon gpionotify` (libgpiod 2), `micropython` (wrapper) and `micropython.bin`, `pioasm`, `ws2812` (extras/ws2812) |
| `usr/sbin` | `i2cdetect i2cget i2cset i2cdump i2ctransfer eeprog` (i2c-tools), `spidev_test` |
| `usr/lib` | `libgpiod.so*`, `libgpiotools.so*`, `libi2c.so*`, `libpio.so`; `micropython/machine.mpy`, `micropython/rp2.mpy` |
| `usr/share/examples/pins` | `gpio-blink gpio-edge i2c-scan spi-xfer adc-read`, built (FDPIC) and as source |
| `usr/share/examples/pio` | `squarewave pio-fifo pio-dma pio-edges ws2812`, built (FDPIC) and as source, `ws2812.pio`, `pio_blink.py pio_echo.py pio_ring.py` |
| `usr/libexec/pins-tests` | `board-test.sh`, `pinpeek`, `irq_test.py`, `pio-selftest` and `rp2_selftest.py` (tests/pio) |
| `lib/libgcc_s.so.1` | stripped, for NPTL's thread exit (see Known issues) |

About 1.4 MB.

## Testing on the board

The squashfs root carries the tree:

```sh
pins/run-build.sh                          # out/pins-root
./run-build.sh all                         # the root filesystem with it
./load.sh
# on the board:
/usr/libexec/pins-tests/board-test.sh
```

`board-test.sh` needs no second board: it uses unwired free pins (GP20,
GP21, GP26) and the SPI0 loopback jumper (GP19 to GP16). `pinpeek` reads a
pin's IO_BANK0/PADS/SIO registers, and measures frequency and duty from the
pad, so PWM and PIO outputs are checked from the board itself. pinpeek
can read a PIO or PWM output low (missed edges while the sampler is off
the CPU), so `board-test.sh` measures up to 3 times.

It covers:

- GPIO: `gpiodetect`, `gpioinfo` names and reserved lines (`PSRAM_CS`
  refused), `gpioset` 1/0 read back from the pad and SIO, `gpioget` with
  pull-up and pull-down, `gpio-edge` (edge events through the GPIO v2
  uAPI), `gpio-blink`
- I2C: `i2cdetect -y 1` on an empty bus (every address `--`), `i2cget` to
  an absent address fails
- SPI: `spidev_test` and `spi-xfer` through the GP19 to GP16 jumper
- ADC: `in_temp_raw`, `adc-read`, MicroPython `ADC(ADC.CORE_TEMP).read_temp()`
- PWM: sysfs GP26 at 1 kHz 25% measured from the pad; MicroPython `PWM`
  at 2 kHz 25%
- PIO: `squarewave 20 1000` measured from the pad, `pio-fifo`, `pio-dma`,
  `ws2812`, MicroPython `pio_echo.py`, `pio_ring.py` and `rp2_selftest.py`
- PIO from ATRANS programs: `atrans-pio`, the ATRANS examples, the tcc
  builds, and no Oops in the kernel log
- MicroPython `Pin` output, `Pin.irq` (20 of 20 edges)
- `pio-selftest` (tests/pio): GPIOBASE 16, pin claims and refusals, reset
  between owners, mmap limits, DMA refusals, DMA echo, a stop from another
  thread ending `DMA_WAIT`, starts racing stops

Nothing here needs a second board, so no I2C device answers in these
checks, and inputs are driven only by the pads' pulls.

### Known issues

- **uClibc-ng FDPIC threads**: a thread created after another thread of the
  same process has exited crashes (UsageFault INVSTATE, a jump to
  ld-uClibc's GOT at offset 0x6f64), seen with MicroPython (`_thread`, one
  thread exiting then another starting) and in `pio-selftest`'s last check
  ("starts racing stops"). One thread
  that exits, or several that stay alive, work. Probably the NPTL stack
  cache's reuse path. `Pin.irq`'s thread lives until the program ends, so it
  is not affected unless a program sets and clears handlers repeatedly.
- **Thread stacks**: uClibc's default thread stack (2 MiB with the default
  8 MiB `ulimit -s`) is more than nommu finds in one piece: C programs with
  threads need `ulimit -s 256` (or `pthread_attr_setstacksize`).
  MicroPython sets its own.
- **MicroPython needs 512 KiB in one piece**: nommu copies
  `micropython.bin` (438 KB) out of the squashfs root into one allocation,
  rounded up to a power of two. After many processes the memory can be too
  fragmented for it (`Allocation of length 438272 ... failed`); right after
  boot it starts.
- **libgcc_s**: NPTL loads it for thread exit/cancel; `post-build.sh` drops
  it, so the tree carries a stripped copy (`lib/libgcc_s.so.1`, 100 KB).
