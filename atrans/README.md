# ATRANS userland: toolchain and programs

ATRANS processes are static programs that get their own address space on
native RP2350 Linux: a PSRAM block shown at 0x11000000 through QMI ATRANS4,
with real `fork()` ([../docs/atrans.md](../docs/atrans.md)).
This directory builds the cross toolchain for them and the system's
interactive programs (tmux, Dropbear, nano, Lua, the extras, claude-guest-mcp),
and assembles them into a tree to overlay on the native root filesystem.

## Build

```sh
atrans/run-build.sh            # toolchain + packages, extras, then the tree
atrans/test-qemu.sh            # smoke tests under qemu-arm (in Docker)
```

Stages: `toolchain` (Buildroot: gcc, musl, the packages; under an hour
the first time), `extras` (extras/ programs, the tests, and claude-guest-mcp
with the host's Rust), `root` (the tree and its tarballs), `shell` (a shell
in the container with the toolchain on `PATH`) and `make ARGS` (make in the
Buildroot output, e.g. `make tmux-rebuild`). `busybox-atrans` is built from
the busybox source the native rootfs build downloads, so `./run-build.sh
rootfs` comes first.

Everything runs in the build's Docker image (`rp2350-native-build`,
`docker/Dockerfile`) with the Buildroot checkout in `src/`. The trees are in
the work volume (`rp2350-native-work`, or `WORK_VOLUME`): `/work/atrans-buildroot` (a patched copy of Buildroot),
`/work/atrans-br` (the output; the toolchain is in `host/bin`) and
`/work/atrans-extras`.

## The toolchain

`arm-atrans-linux-musleabi-gcc`, also called `atrans-gcc` (GCC 15.3, binutils
2.45.1, musl 1.2.6, Linux 6.12 headers). In the container:

```sh
atrans-gcc hello.c -o hello                          # an ATRANS v3 program
atrans-gcc -Wl,--defsym=__atrans_stack=0x8000 ...    # 32 KiB of stack instead of 128 KiB
atrans-gcc -T my.ld ...                              # another link script instead of atrans-v3.ld
```

Its defaults give binaries that follow the ATRANS v3 contract with no
extra flags:

- **Thumb-2 for ARMv7E-M, soft-float EABI.** GCC is configured with
  `--with-cpu=cortex-m4 --with-mode=thumb --with-float=soft` (the M33 runs
  ARMv7E-M code; the existing FDPIC userland uses the same CPU). libgcc,
  libatomic, musl and every package are built that way.
- **Static.** `--with-specs=%{!shared:%{!r:-static}}`; only static
  libraries exist.
- **The v3 layout: shared text.** `build.sh` puts `atrans-v3.ld` in the
  toolchain's sysroot (`usr/lib`) and a `specs` file in GCC's library
  directory that adds `-T <sysroot>/usr/lib/atrans-v3.ld` to every final
  link without its own `-T`. The script is the kernel side's
  `tests/atrans-v3.ld` with two changes:
  - Text, rodata, `.ARM.exidx` and `.eh_frame` go at 0x11C00000, in the
    text window. That segment is read-only with no bss, so the kernel
    loads it once per binary and every process running it shares it.
  - The data window at 0x11000000 holds the stack region, then the ELF
    headers, `.tdata`, the init arrays, `.data` and `.bss`. The heap
    follows at run time. The headers are loaded with the data, so
    `AT_PHDR` points at real memory under qemu-user too.

  The stack region is [0x11000020, data start): 128 KiB by default, as in
  v2. The kernel's own default (64 KiB) is too small for ncurses: on the
  board, `tput` overflows it (the kernel kills it for a bad stack
  pointer), and qemu-user does not notice because its stack is large.
  `-Wl,--defsym=__atrans_stack=SIZE` changes it anywhere on the command
  line (the script `PROVIDE`s the default). SIZE must be a multiple of
  4 KiB, at least 8 KiB. Pages are 4 KiB.
- **v2 is still there.** The binutils patch moves ld's built-in text
  segment from 0x10000 to 0x11020000: one window, 128 KiB of stack below
  the image. That is what `ld` gives without `-T`. A v2 kernel only runs
  v2 binaries. A v3 kernel runs both.

The patches (`patches/`, applied through `BR2_GLOBAL_PATCH_DIR`):

| Package | Change |
|---|---|
| musl | On M profile, `__get_tp()` and `__aeabi_read_tp` read the thread pointer from the word the ATRANS v2 kernel keeps at 0x11000000. That word is readable but not writable from user mode, and the kernel rewrites it on `set_tls` and on every switch. `__set_thread_area` turns this on only when the process has the ATRANS layout (sp inside the window, below `AT_PHDR`) and the word holds the pointer just set. Anywhere else, qemu-user for example, they use the `get_tls` syscall (0xf0006), with r7 saved in ip around the `svc`. The plain time syscall names become the `*_time64` ones, as on time64-only arches: otherwise musl tries the time32 calls first, which don't exist without `COMPAT_32BIT_TIME`. setjmp/longjmp skip the FPA/VFP/iWMMXt code, `atomics.s` drops its cp15 and kuser variants, and `tlsdesc.S` calls `__aeabi_read_tp`. musl's ARMv7 atomics are already ldrex/strex with `dmb`; clone, vfork, syscall_cp and sigsetjmp need no change. |
| gcc (libgcc) | The `__sync_*` helpers use ldrex/strex instead of the kuser helpers at 0xffff0fc0. The 64-bit helpers use a spinlock, since ARMv7-M has no ldrexd. The unwinder's VFP/iWMMXt save/restore routines are empty on soft-float M profile. libatomic needs no change, because it has no kuser path when ifuncs are off. |
| binutils | ld's built-in link address, the v2 layout (above). |

`build.sh` also patches its copy of Buildroot so that ARMv7-M may select
`BR2_USE_MMU`. That gives ELF binaries and enables the packages that need
`fork()`. It also removes the "no static" restriction from lrzsz (a uClibc
clash) and htop (dlopen, only for libsensors).

`check-atrans.py FILE...` checks binaries against the contract:

- ELF32 `ET_EXEC` `EM_ARM` EABI5 with a soft-float ABI
- no `PT_INTERP` or `PT_DYNAMIC`
- every `PT_LOAD` inside the data window [0x11000000, 0x11400000) or the
  text window [0x11C00000, 0x12000000)
- text-window segments read-only with `p_filesz == p_memsz`, writable
  ones in the data window
- a stack region of at least 8 KiB, and a Thumb entry point
- no `$a` mapping symbols (no ARM-mode code)
- ARMv7-M build attributes, with no FP or SIMD
- no VFP, NEON or coprocessor instruction (cp15 included), no undefined
  encoding and no kuser address in the disassembly

`install-root.sh` runs it on every program before stripping and writes the
results to `out/atrans-check.txt`. All the programs pass.

## The tree: `out/atrans-root/`

Also packed as `out/atrans-root.tar.gz` (owner root).
Unpack or copy it over the native root filesystem before making the
squashfs, for example in a Buildroot post-build script:

```sh
tar -xzf out/atrans-root.tar.gz -C "$TARGET_DIR"
```

| Path | Contents |
|---|---|
| `usr/bin` | tmux, nano, mg, less, bc, dc, ncdu, lua, luac, htop, rz/sz (lrz/lsz/rb/sb/rx/sx), clear, tput, tset/reset; Dropbear's client applets (`dbclient`, `ssh`, `scp`, `dropbearkey`, `dropbearconvert`, symlinks to `../sbin/dropbear`); `xauth` (`xauth.c`: add, remove and list, for dropbear's X11 forwarding); `claude-remote`, `claude-guest-mcp` |
| `usr/sbin/dropbear` | Dropbear, one multi-call binary. It is built with public-key logins only, no re-exec per connection, and X11 forwarding (`dropbear-localoptions.h`). |
| `etc/init.d/S50dropbear`, `etc/default/dropbear`, `etc/dropbear -> /home/dropbear` | SSH: host keys are made at the first connection and kept in `/home/dropbear`, so they last across reboots. |
| `usr/share/terminfo` | xterm, xterm-256color, tmux, tmux-256color, screen, screen-256color, vt100/102/220, linux, ansi, putty, dumb |
| `usr/libexec/atrans-tests` | `hello`, `selftest`, `run-tests.sh` (below) |
| `usr/lib/atrans` | The sysroot for building on the board (below) |

The overlay replaces any busybox applet with the same name (`less`,
`clear`, `reset`, maybe `bc`).

The programs in `extras/` are not in the overlay. They are built on the
board with `cc` ([extras/README.md](../extras/README.md)).

### Sysroot for `cc` on the board: `/usr/lib/atrans`

Also packed as `out/atrans-sysroot.tar.gz`. It holds:

- `include/`: all of musl's headers, the ncurses headers and the kernel UAPI
  headers they include (plus linux/gpio, i2c-dev, spidev, input, fb, if,
  random)
- `lib/`: `crt1.o crti.o crtn.o rcrt1.o`, `libc.a` (musl puts libm, libpthread,
  librt, libdl and the others into it; their `.a` files are empty
  placeholders), `libgcc.a`, `crtbeginT.o crtend.o`, `libncursesw.a`,
  `libpanelw.a`, `libmenuw.a` and `libformw.a`, with `libncurses.a`
  and the like as symlinks, and `atrans-v3.ld`

A compiler on the board must produce what `atrans-gcc` produces:

- Thumb-2 for ARMv7-M, soft-float
- static, `ET_EXEC`, the v3 layout: read-only segments at 0x11C00000,
  writable ones at 0x11000000 + the stack size (or v2: everything from
  0x11020000)
- the link order `crt1.o crti.o crtbeginT.o <objects> --start-group -lgcc -lc --end-group crtend.o crtn.o`, as gcc uses

The thread pointer is `__aeabi_read_tp` (in libc.a). The debug
information is stripped, which leaves 4.7 MB of libraries and 1.5 MB of
headers.

## Sizes and memory

Stripped file sizes are from `out/atrans-sizes.txt`. Text and
data come from `atrans-check.txt`.

Under v3 a process costs two things:
- **Shared text** (text and rodata, the "Text" column). It is loaded once
  per binary, however many processes run it.
- **Its own block**: the stack region (128 KiB by default), then data and
  bss (the "Data" column), then the heap and mmaps.

For example, three `lua` processes on the board share one 188 KiB text
block, and each has a 200 KiB block of its own. Under v2, every block also
holds the whole image.

| Program | File | Text (shared) | Data+bss | Notes |
|---|---|---|---|---|
| tmux | 779 KB | 748 KiB | 44 KiB | plus libevent buffers; the server and each client are separate processes |
| dropbear (multi-call) | 288 KB | 277 KiB | 8 KiB | a forked child per connection, plus a shell |
| nano | 293 KB | 282 KiB | 10 KiB | |
| htop | 356 KB | 339 KiB | 16 KiB | |
| less | 256 KB | 237 KiB | 60 KiB | |
| mg | 256 KB | 238 KiB | 22 KiB | |
| ncdu | 247 KB | 237 KiB | 11 KiB | its heap grows with the tree it scans |
| lua / luac | 197 / 129 KB | 188 / 122 KiB | 7 / 5 KiB | |
| bc / dc | 73 / 54 KB | 67 / 48 KiB | 5 / 5 KiB | |
| rz / sz | 96 / 90 KB | 90 / 83 KiB | 6 / 7 KiB | |
| clear, tput, tset | 103–111 KB | 96–104 KiB | 4–6 KiB | ncurses' terminfo reader |
| demos | 20–53 KB | 16–48 KiB | 2–5 KiB | |
| claude-guest-mcp | 39 KB | 34 KiB | 1 KiB | plus its 128 KiB arena (mmap) and the shells it runs |
| hello | 9 KB | 4 KiB | 2 KiB | |

The whole tree is 11 MB on disk: about 4.6 MB of programs, and the
sysroot (6.3 MB of libraries and headers). The tarball is 4.1 MB, and the
sysroot tarball alone is 1.1 MB.

## Tests

`test-qemu.sh` runs `tests/run-tests.sh ssh` under qemu-arm 7.2 user mode
in Docker. It uses a Cortex-A15 executing the Thumb-2 code. qemu is
registered for ARM ELF files in binfmt_misc, so the programs can fork and
exec each other.

The run covers:

- `hello`
- `selftest`:
  - layout and auxv
  - malloc
  - fork, exec, vfork, `posix_spawn`, `system` and `popen` (musl uses
    `clone(CLONE_VM|CLONE_VFORK)` on a new stack)
  - pthreads with TLS and errno per thread, mutex and condvar
  - signals, including SIGSEGV with `siglongjmp`; setjmp
  - mmap; pipes with poll; ptys
  - 32- and 64-bit atomics; soft-float; clocks
- Lua, bc, dropbearkey (ed25519 and ECDSA), the version banners of nano,
  less, ncdu, htop and lsz, and tput
- claude-guest-mcp running a shell command
- tmux: new-session, send-keys, capture-pane and list-sessions
- a Dropbear server with a dbclient login over 127.0.0.1, both without a
  pty and with one (dbclient inside tmux)

On the board, run the same script there:

```sh
/usr/libexec/atrans-tests/run-tests.sh        # add "ssh" to log in to itself over 127.0.0.1
```

The `ssh` test adds a key to `~/.ssh/authorized_keys` for the duration and
then restores the file.

`selftest` marks two checks SKIP under qemu, because qemu-user lays out the
process differently: the stack below the image, and `AT_HWCAP` == 0. They
must PASS on the board.

qemu-user can't show two kinds of fault, because its stack is large and
it has no MPU:
- a stack that is too small: the kernel kills the process for a bad
  stack pointer
- pointer checks in the kernel's system calls

Only the board finds these.

The kernel's `rp2350_atrans_access_ok` accepts zero-length accesses, as
stock Linux does: musl writes a fully buffered stream, and
`fprintf(stderr)`, with `writev` and a `{NULL, 0}` iovec, which would
otherwise fail with EFAULT and lose the output.

### Measured on the board

- `selftest`: 34 of 34 (v3 layout, `AT_PHDR` 0x11010034).
- Three lua processes share one 188 KiB text block (`/proc/atrans`:
  `3 users`).

## claude-guest-mcp

`claude-remote/` builds claude-guest-mcp for Arm. `src/sys.rs` has the Arm
EABI system call table, and `src/main.rs` a Thumb `_start`. `link-arm.x` gives it the v3 layout with explicit `PHDRS`:
code and constants in one R+X segment at 0x11C00000, and the headers,
data and bss in a RW segment at 0x11020000 (128 KiB of stack), which comes
first in the file. lld's page size is set to 4 KiB. On Arm the small-block arena is
128 KiB instead of 256 KiB. `run-build.sh extras` runs `cargo build
--target thumbv7m-none-eabi`.
