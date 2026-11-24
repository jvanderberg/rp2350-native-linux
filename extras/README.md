# extras

Programs for the board: games and demos, a telnet BBS, two file managers,
a Midnight Commander lookalike, a terminal multiplexer and a WS2812 LED
driver.

There are two ways to build them:

- `atrans/build-extras.sh` cross-builds them with `atrans-gcc` (the
  `extras` stage of `atrans/run-build.sh`) into the ATRANS tree that goes
  into the image: the programs in `/usr/bin` (the demo `httpd` as
  `demo-httpd`) and the BBS in `/usr/share/bbs`. `ws2812` is not among
  them; it is in `/usr/share/examples/pio`.
- On the board, `cc` (tcc) builds the same sources with the commands
  below. `tcc/extras.sh` checks that they build with the cross tcc.

| Directory | What it is | Build on the board and run |
|---|---|---|
| [`demos/`](demos/) | Terminal games and demos | `./build.sh`, then `./asteroids` |
| [`bbs/`](bbs/) | A telnet BBS with a MUD | `./bbsctl start` |
| [`mc/`](mc/README.md) | A Midnight Commander lookalike | `./build.sh && ./mc` |
| [`tm/`](tm/README.md) | A small tmux work-alike | `./build.sh && ./tm` |
| [`nnn/`](nnn/) | The nnn file manager | `./build.sh && ./nnn` |
| [`noice/`](noice/) | The noice file browser | see below |
| [`ws2812/`](ws2812/) | WS2812 LEDs driven by PIO | see below |

In the examples, lines starting with `#` are in the board's shell, and
lines starting with `$` on the host.

## Putting the sources on the board

To build on the board, copy the sources there. `/home` holds only 1 MB, so
use the SD card. With the card mounted at `/mnt/sd`:

```
$ tar cf - -C extras . | ssh root@ADDRESS 'mkdir -p /mnt/sd/extras && tar xf - -C /mnt/sd/extras'
```

`nnn` uses the curses headers in `noice/include`, so copy `noice/` with it.

## demos

`asteroids`, `stackfall` (falling blocks), `life`, `rain` (digital rain),
`pulse` (CPU, memory, network and processes), `bogomips`, `maze` (a
first-person 3D maze; `./maze -demo` explores it by itself), `mandel` and a
small `httpd`. `./build.sh` builds the first seven and the two X11 programs
below, or only the ones named (`./build.sh life rain`). Build the others with
`cc`:

```
# cc -o mandel mandel.c
# cc -o httpd httpd.c
```

`xeyes` and `xmaze` open a window on an X server instead of using the
terminal, such as XQuartz on a Mac. `xmaze` is the maze drawn as X shapes
rather than pixels, about 1 KB a frame, so it stays fast over the board's
network link and sharp at any window size; it takes `-demo` too. Both use
`x11.h`, which speaks the X11 protocol itself, so no Xlib is needed; it is
the window counterpart of `term.h`. On the machine with the screen, allow
network clients (XQuartz: Settings > Security) and run
`xhost +BOARD-ADDRESS`, then on the board:

```
# DISPLAY=SCREEN-ADDRESS:0 ./xmaze -demo
```

## bbs

A telnet BBS with message boards, chat, XMODEM and YMODEM transfers, and a
MUD set inside the RP2350 (`mud/*.txt`). `./bbsctl start` builds it and
starts `inetd` on `telnet_port` from `bbs.conf`; `./bbsctl stop` ends it.
Port 23 is the board's own telnet, so stop that or pick another port. Run
`./bbsctl start` again after each reboot.

## noice

```
# cc -Iinclude -o noice noice.c strlcat.c strlcpy.c -lncurses && ./noice
```

## ws2812

The same program as `ws2812` in `/usr/share/examples/pio`:

```
# pioasm ws2812.pio ws2812.pio.h && cc -o ws2812 ws2812.c -lpio
# ./ws2812 PIN COUNT RRGGBB
```

## Licenses

`demos/`, `bbs/`, `mc/` and `tm/` are under the repository's MIT license.

- `nnn/` and `noice/` are ports of upstream nnn and noice (BSD-2-Clause,
  see their `LICENSE` files), with small shims so they build with tcc.
  `noice/include` holds ncurses headers (MIT-X11).
- `ws2812/ws2812.pio` comes from the pico-examples (Copyright (c) 2020
  Raspberry Pi (Trading) Ltd., BSD-3-Clause).
