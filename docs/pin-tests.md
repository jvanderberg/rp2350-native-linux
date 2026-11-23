# Pin tests with a second board

`tools/pin-test.py` tests the Linux board's pins against a second RP2350B
board running `tools/pintester/`, which drives and measures them. It ends
with `N of M passed` and exits with 0 only when every check passed.

## tools/pintester/ (RISC-V, Rust, rp235x-hal)

Firmware for the second board, which runs it on its Hazard3 cores: GPIO
levels, pulses, frequency and duty measurement, and an I2C target, driven
over USB serial (`/dev/cu.usbmodempintest1`). Each command is one line, and
every reply ends with a line `ok` or `err WHY`.

| Command | Does |
|---|---|
| `id` | prints `pintester` |
| `out P V` | GP`P` an output at `V` (0 or 1) |
| `in P [u/d/n]` | GP`P` an input with a pull-up, a pull-down or neither (the default); prints `P=V` |
| `get P` | GP`P`'s level, `P=V` |
| `pulse P N US` | toggles output GP`P` `N` times, `US` microseconds apart |
| `meas P MS` | watches GP`P` for `MS` ms: `rises=R falls=F hz=H duty=D` (duty in %) |
| `regs` | the I2C target's first 16 registers |
| `i2c` | the I2C target's counters |
| `bootsel` | reboots into USB BOOTSEL |

The I2C target is always on: I2C1 on GP14 (SDA) and GP15 (SCL), address
0x42, internal pull-ups. It is a 256-byte register file like a 24C02: a
write's first byte sets the register pointer and the rest are stored from
there, and a read returns bytes from the pointer. Opening the port at 1200
baud and closing it also reboots into BOOTSEL.

A board running debugprobe ignores `picotool reboot -f -u` and the
1200-baud reboot, so put it in BOOTSEL by hand the first time (hold
BOOTSEL while plugging it in). Once it runs the tester, its `bootsel`
command does it.

```sh
cd tools/pintester && cargo build --release
picotool load --ser SERIAL -v -x -t elf target/riscv32imac-unknown-none-elf/release/pintester
```

`tools/pintester/debugprobe-v2.3.1-backup.uf2` is Raspberry Pi's debugprobe
2.3.1, kept so a board used as the tester can be returned to being a debug
probe: `picotool load --ser SERIAL -v -x debugprobe-v2.3.1-backup.uf2`. It
is Raspberry Pi's code and others' (MIT, BSD-3-Clause, Apache-2.0), not this
project's: see `tools/pintester/debugprobe-v2.3.1-backup.NOTICE.txt`.

## Wiring

The two boards share a ground. The tester's pin for each wire is in the
header of `tools/pin-test.py`, which is the reference.

| Linux board | Goes to | Used for |
|---|---|---|
| GP13 | tester GP10 | GPIO output; PIO output (square waves, ws2812, MicroPython blink) |
| GP14 | tester GP11 | GPIO input and edge events; PIO edge capture |
| GP15 | tester GP12 | PWM (sysfs and MicroPython) |
| GP3 | tester GP13 | the pull-up test |
| GP4, GP5 | tester GP14, GP15 | I2C0 SDA, SCL (the tester is the target at 0x42) |
| GP19 | GP16, on the Linux board | SPI0 loopback, MOSI to MISO (a jumper) |

## Running

```
$ tools/pin-test.py [--host ADDR] [--only NAME,...]
```

- The Linux board is reached by `ssh root@ADDR`, with key login. Without
  `--host`, the script asks the board's console for `eth0`'s address with
  `tools/board-cmd.py`, so the console port must be free (close `screen`).
- The tester is the first `/dev/cu.usbmodempintest*`.
- First it checks that the tester answers, and builds two helpers on the
  board with tcc (a GPIO v2 edge counter and toggler, and an SPI loopback
  test). Then it runs the sections, all of them or the ones `--only` names,
  and prints a `PASS` or `FAIL` line for each check (`INFO` lines are
  reported only).

| Section | Checks |
|---|---|
| `gpio` | GP13 driven 1/0/1/0 with `gpioset`, read by the tester; GP14 driven by the tester, read with `gpioget` |
| `pull` | GP3's pull-up (`gpiomon -b pull-up`) raises the line once the tester lets go of it |
| `edges` | GP14 edge events through the GPIO v2 uAPI: all 40 at 20 ms and all 100 at 5 ms, rising and falling in turn. 200 at 2 ms and at 1 ms, and GP13 toggled every 25 ms, are reported only (`INFO`). |
| `pwm` | GP15 by sysfs at 1 kHz 25%, 10 kHz 50%, 100 Hz 75% and 1 MHz 50%, measured within 1% and 2 points of duty; then off |
| `i2c` | `i2cdetect` finds 0x42; `i2cset`/`i2cget` bytes and words; `i2ctransfer` with a repeated start; an absent address fails; MicroPython `I2C` |
| `spi` | SPI0 loopback in modes 0-3, at 100 kHz to 50 MHz, 1 to 4096 bytes; MicroPython `SPI` at 10 MHz |
| `micropython` | `Pin` output, `Pin.irq` catching 20 edges at 20 ms, `PWM` at 2 kHz 25% |
| `pio-self` | `pins/tests/pio` on the board, with no wiring of its own (it uses free pins GP15, GP20, GP21, GP31, GP33): libpio and MicroPython `rp2` at GPIOBASE 0 and 16, read back by the other block or driven from Linux, `WAIT GPIO`'s index, outputs reset between programs, the driver's refusals, and DMA (echo, a stop ending a wait, starts racing stops) |
| `pio` | builds the PIO examples on the board (tcc and `pioasm`); `squarewave` on GP13 at 1 kHz, 100 kHz and 1 MHz, within 1% and 50 ± 3% duty; `ws2812` sends 240 bits for 10 LEDs; `pio-edges` catches all of 200 edges at 1 ms and of 2000 at 20 µs and at 5 µs on GP14, with the right mean spacing; `pio-fifo`; `pio-dma` (echo and ring); MicroPython `pio_blink.py` at 23.7 Hz, `pio_echo.py`, `pio_ring.py` |

Pull-downs aren't tested: on the RP2350 a line that has been high stays near
2 V against the internal pull-down (erratum RP2350-E9).
