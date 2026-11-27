# extras

Programs for the board: terminal games and demos, X11 programs, a telnet
BBS, a Midnight Commander lookalike, a terminal multiplexer, a terminal in
an X11 window, Wolfenstein 3D, a WS2812 LED driver, and ZIP archive tools.

The extras are available in the separate release bundle. You can also
copy most programs' sources to the board and build them there with `cc`
(tcc), using the commands below. On the host, `tcc/extras.sh` checks that
those programs build with the cross tcc.

| Directory | What it is | Build on the board and run |
|---|---|---|
| [`games/`](games/) | `asteroids`, `stackfall` (falling blocks), `life`, `rain` (digital rain) | `./build.sh`, then `./asteroids` |
| [`maze/`](maze/) | A first-person 3D maze, in the terminal and in an X11 window | `./build.sh && ./maze` |
| [`pulse/`](pulse/) | CPU, memory, network and processes, in the terminal and in an X11 window | `./build.sh && ./pulse` |
| [`samples/`](samples/) | Small samples: `bogomips`, `mandel`, `xeyes`, `xgallery` | `./build.sh && ./mandel` |
| [`httpd/`](httpd/README.md) | A small static file server | `./build.sh && ./httpd` |
| [`tm/`](tm/README.md) | A small tmux work-alike | `./build.sh && ./tm` |
| [`xterm/`](xterm/README.md) | A terminal in an X11 window | `./build.sh`, see its README |
| [`xedit/`](xedit/README.md) | An X11 text editor with menus and file dialogs, for files larger than RAM | `./build.sh && ./xedit [filename]` |
| [`vim/`](vim/README.md) | Vim's tiny terminal build with core help | `./build.sh && ./vim filename` from the extras ZIP |
| [`mc/`](mc/README.md) | A Midnight Commander lookalike, and `xmc`, the same in an X11 window | `./build.sh && ./mc` |
| [`bbs/`](bbs/) | A telnet BBS with a MUD | `./bbsctl start` |
| [`wolf3d/`](wolf3d/README.md) | Wolfenstein 3D in an X11 window | see its README |
| [`ws2812/`](ws2812/) | WS2812 LEDs driven by PIO | `./build.sh`, see below |
| `zip/` | Create, list, test and extract ZIP archives | Prebuilt in the release bundle |
| [`lib/`](lib/) | Code the others share (below) | |

In the examples, lines starting with `#` are in the board's shell, and
lines starting with `$` on the host.

## Putting the sources on the board

To build on the board, copy the sources there. `/home` holds only 1 MB, so
use the SD card. With the card mounted at `/mnt/sd`:

```
$ tar cf - -C extras . | ssh root@ADDRESS 'mkdir -p /mnt/sd/extras && tar xf - -C /mnt/sd/extras'
```

Keep the layout: most programs include headers from `lib/` by a relative
path (`#include "../lib/x11.h"`), so `lib/` has to be next to them.

## Building

Each directory's `build.sh` builds all its programs, or only the ones
named (`games/build.sh life rain`). It compiles one file at a time, which
keeps tcc's memory low enough for the board, and links to `NAME.new`
before renaming it, so a program can be rebuilt while it runs.
`CC=gcc ./build.sh` builds with another compiler.

A program in one file can also be built by hand, with no `-I`:

```
# cc -o mandel mandel.c
```

## games, maze, pulse and samples

`asteroids`, `stackfall`, `life` and `rain` run in the terminal, through
`lib/ansi.h`. `maze` is a first-person 3D maze; `./maze -demo` explores
it by itself. `pulse` shows CPU, memory, network and processes. `bogomips`
and `mandel` just print.

`xeyes`, `xmaze` and `xpulse` open a window on an X server instead of using
the terminal, such as XQuartz on a Mac. `xmaze` is the maze drawn as X shapes
rather than pixels, about 1 KB a frame, so it stays fast over the board's
network link and sharp at any window size; it takes `-demo` too. `xpulse` is
`pulse` in a window, with graphs, a process table sorted by clicking a
column, and a right-click menu to signal a process; an update sends only
what changed, 1-4 KB. All three use `lib/x11.h`, which speaks the X11
protocol itself, so no Xlib is needed; it is the window counterpart of
`lib/ansi.h`. Where the X server has the RENDER extension (XQuartz and Xorg
do), `x11.h` also draws anti-aliased text in TrueType fonts (the board has
DejaVu Sans and DejaVu Sans Mono in `/usr/share/fonts/dejavu`), shapes with
smooth edges and see-through colours, and has smooth cursors and the
clipboard; `xgallery` shows them all. On a server with the HIDPI extension
([crystalX](https://github.com/jvanderberg/crystalx) on a high-density
screen) the text is made at the screen's own resolution rather than
enlarged; `X11_HIDPI=0` turns that off. The simplest way to run one is
through SSH's X11 forwarding, from the machine with the screen:

```
$ ssh -Y root@BOARD-ADDRESS /mnt/sd/extras/maze/xmaze -demo
```

Or, without SSH: on the machine with the screen, allow network clients
(XQuartz: Settings > Security) and run `xhost +BOARD-ADDRESS`, then on the
board:

```
# DISPLAY=SCREEN-ADDRESS:0 ./xmaze -demo
```

Or use [crystalX](https://github.com/jvanderberg/crystalx) in place of
XQuartz: a tabbed terminal for macOS and Wayland with an X server inside,
needing no X installation. `ssh root@BOARD-ADDRESS` from one of its tabs
forwards X by itself, and each program run there opens as a tab next to
it. On a Retina screen it draws at full resolution where XQuartz
enlarges, and `x11.h` sends its text, curves and cursors to suit.

## lib

| File | What it is | Used by |
|---|---|---|
| `ansi.h` | Raw keys, the alternate screen and a frame buffer for ANSI terminals | games, maze, pulse |
| `x11.h` | A small X11 client that speaks the protocol over a socket, with no Xlib: shapes, images, canvases and core fonts, and through RENDER TrueType text, smooth shapes, translucency and cursors; the clipboard | the X11 programs |
| `vt/` | The terminal emulator: `vt.c`, scrollback (`sb.c`, `lz.c`), `vtutil.c`, and `vtpane.h`, a terminal in part of an X11 window | tm, xterm, xmc |

## bbs

A telnet BBS with message boards, chat, XMODEM and YMODEM transfers, and a
MUD set inside the RP2350 (`mud/*.txt`). `./bbsctl start` builds it and
starts `inetd` on `telnet_port` from `bbs.conf`; `./bbsctl stop` ends it.
Port 23 is the board's own telnet, so stop that or pick another port. Run
`./bbsctl start` again after each reboot.

## ws2812

The same program as `ws2812` in `/usr/share/examples/pio`. `build.sh` runs
`pioasm ws2812.pio ws2812.pio.h`, then `cc -o ws2812 ws2812.c -lpio`:

```
# ./build.sh
# ./ws2812 PIN COUNT RRGGBB
```

## zip and unzip

Unpack the extras release on your host and copy the `extras` directory to
the SD card. On the board, add the tools to your shell's path and run them
from a writable directory:

```sh
export PATH="/mnt/sd/extras/zip:$PATH"
cd /mnt/sd
zip -r backup.zip mydir
unzip -l backup.zip
unzip backup.zip -d restored
```

The release cross-builds Info-ZIP Zip 3.0 and UnZip 6.0 with the ATRANS
toolchain using the pinned Buildroot recipes. The release includes their
source archives and the Buildroot patches under `extras/zip/source/`.

## Licenses

Everything here is under the repository's MIT license, except:

- `zip/` in the release bundle contains Zip and UnZip under the Info-ZIP
  license. See `LICENSE` in each source archive.
- `wolf3d/` is a port of Wolf4SDL, under the GNU GPL version 2 (see its
  `PROVENANCE` and `LICENSE-GPL-2.0`); its own additions, `xwolf.c`,
  `SDL.h` and the scripts, are MIT. The game data is not included.
- `ws2812/ws2812.pio` comes from the pico-examples (Copyright (c) 2020
  Raspberry Pi (Trading) Ltd., BSD-3-Clause).
- `vim/` includes upstream Vim under the Vim license and an ncurses header
  under its own license. See [the Vim README](vim/README.md).
