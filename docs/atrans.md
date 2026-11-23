# ATRANS processes

The RP2350 has no MMU, so this kernel is a nommu kernel: one flat address
space, and stock nommu Linux offers only `vfork`. ATRANS processes get
around that. The QMI's address translation (ATRANS4) maps a window at
0x11000000 onto any 4 KiB-aligned stretch of PSRAM, and the kernel points
that window at the running process's own memory on every switch. Each
ATRANS process therefore sees its memory at the same addresses, so it can
have real `fork()`, and the M33's MPU confines it to its window.

Programs are recompiled, not rewritten: an ATRANS program is a static
binary linked at fixed window addresses. The toolchain (`atrans-gcc`, musl)
is in [atrans/](../atrans/README.md), and the board's tcc builds the same
kind of binary. The hardware behaviour this relies on is in
[qmi-atrans.md](qmi-atrans.md).

The driver is `linux/drivers/rp2350/rp2350-atrans.c`, and
`linux/include/asm/rp2350-atrans.h` declares its hooks. The pool it takes
blocks from is `rp2350-atrans-pool.c` ([memory.md](memory.md)); whole-process
swap is `rp2350-atrans-swap.c` ([atrans-swap.md](atrans-swap.md)).

## The ABI

An ATRANS program is a static ELF32 `ET_EXEC`, `EM_ARM`, Thumb-2 ARMv7-M
code, soft-float EABI. Its own binfmt loader runs ahead of ELF-FDPIC. A
binary whose segments lie outside the windows (the FDPIC busybox, for
example) is passed on unchanged. A `PT_INTERP` is refused.

The loader accepts two layouts:

| Layout | Linker script | Windows |
|---|---|---|
| v2: one window | `tests/atrans.ld` | everything in the data window, image at 0x11020000 |
| v3: shared text | `tests/atrans-v3.ld`, `atrans/atrans-v3.ld` (atrans-gcc's default) | data in the data window, text and read-only data in the text window |

A binary with no segment in the text window is a v2 binary. A binary whose
image starts at 0x11000000 is refused with `EINVAL` and a log line saying to
relink at 0x11020000.

### The data window (ATRANS4, 0x11000000–0x113fffff)

| Range | Contents |
|---|---|
| 0x11000000 – 0x1100001f | Thread pointer word: the current thread's TLS pointer. Read-only for user mode (MPU region 1). |
| 0x11000020 – lowest data `p_vaddr` | Stack, which grows down. Its size is a link-time choice (below). |
| image | data and bss (v3), or text, data and bss (v2) |
| `start_brk` (page after the image) – `brk` | heap |
| between `brk` and the block top | `mmap` pages |
| block top – 0x11400000 | not backed: an access is a MemManage fault, so SIGSEGV |

### The text window (ATRANS7, 0x11c00000–0x11ffffff)

A v3 binary's read-only part (text, rodata, exidx) goes in a second window.
The kernel loads it once per binary and every process running that binary
shares it, so a further process running the same binary costs only its
data, and `fork` never copies text.

### Linking

- `tests/atrans-v3.ld` places `.text`, `.rodata`, `.ARM.exidx` and
  `.eh_frame` at 0x11c00000, and the ELF headers, `.tdata`, the init arrays,
  `.data` and `.bss` at 0x11000000 plus the stack size. Loading the headers
  makes `AT_PHDR` real memory under qemu-user too.
- The stack size is set with `-Wl,--defsym=__atrans_stack=SIZE` anywhere on
  the command line. It must be a multiple of 4 KiB and at least 8 KiB. The
  kernel tests' script defaults to 64 KiB; atrans-gcc's copy
  (`atrans/atrans-v3.ld`) defaults to 128 KiB, because ncurses' terminfo
  reader needs about 70 KiB. qemu-user's large stack hides a stack that is
  too small.
- The board's tcc links the v3 layout by default, with a 128 KiB stack
  (`tcc/patches/0017-*`); `-Wl,-Tdata=ADDR` moves the writable segments,
  and `-Wl,-Ttext=0x11020000` gives the v2 layout.
- v2 programs set their stack size by their link address: the default
  0x11020000 gives 128 KiB; 0x11008000 gives 32 KiB.

### Loader rules

- `PT_LOAD`s in the text window must be read-only with
  `p_filesz == p_memsz` (no bss). Otherwise exec fails with `EINVAL`.
- Writable `PT_LOAD`s go in the data window. The stack region is
  [0x11000020, lowest data `p_vaddr`).
- A program with no data segment gets a 64 KiB stack and its heap after it.
- **Initial stack:** the Linux ELF ABI, sp 8-aligned just below the image.
  From the top down:
  - the strings (argv, envp, file name)
  - 16 `AT_RANDOM` bytes
  - a copy of the program headers, if no `PT_LOAD` holds them
  - argc, argv, envp
  - auxv: `AT_PAGESZ`, `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`, `AT_BASE` 0,
    `AT_FLAGS`, `AT_ENTRY`, `AT_HWCAP` 0, `AT_CLKTCK`,
    `AT_UID`/`EUID`/`GID`/`EGID`, `AT_SECURE`, `AT_RANDOM`, `AT_EXECFN`,
    `AT_NULL`.

### Syscalls

- ARM EABI: `svc 0`, number in r7.
- `fork` (2), `vfork` (190), `clone` (120). `clone` without `CLONE_VM` is a
  fork; with `CLONE_VM|CLONE_VFORK` it is a vfork; with
  `CLONE_VM|CLONE_THREAD|CLONE_SETTLS` it creates threads that share the
  block.
- `execve` between FDPIC and ATRANS programs, either way.
- `mmap2`, `munmap`, `brk`.
- **TLS:** `set_tls` (0xf0005) or `CLONE_SETTLS` sets the thread pointer,
  and `get_tls` (0xf0006) returns it in r0. The kernel also keeps the
  current thread's pointer in the word at 0x11000000, rewriting it on
  `set_tls` and on every switch to an ATRANS thread, so `__aeabi_read_tp`
  can be `mov r0, #0x11000000; ldr r0, [r0]; bx lr`.
- **mmap:** every mapping is a private copy, and `prot` is not enforced.
  Writable `MAP_SHARED` returns `EINVAL`. `mremap` fails, and `mprotect`
  returns `ENOSYS`, as on all nommu kernels; musl copes with both.
- **Out of memory:** `brk`, `mmap`, `fork` and `execve` return `ENOMEM`.
  These allocations never invoke the OOM killer.

## How it works

### Blocks

- **One block per process.** Each process owns one physically contiguous
  PSRAM block. ATRANS4 maps it at the window: BASE is the block and SIZE is
  its length. So the block always covers [0x11000000, 0x11000000 + size),
  and the whole stack region is resident.
- **Initial size:** the image plus 64 KiB of heap.
- **Growth:** `brk` past the block top, `mmap` with no free run, or a
  `MAP_FIXED` past the top. The block grows by half again plus 32 KiB of
  slack, up to the 4 MiB window. Window addresses never change.
  - **In place,** when the space right after the block is free (in the
    pool, blocks next to it can be slid away to make it so). One cache flush
    and a new SIZE follow. There is no copy and no second block.
  - **Otherwise by moving:** a new block, a copy of what is in use from the
    cached window to the new block's uncached alias, a flush, an ATRANS4
    write, and the old block freed. The copy runs without sleeping, so no
    other task (or thread of the process) can touch the window meanwhile.
- **Shrinking:** after `munmap` or a `brk` shrink, the block gives back its
  top in place when at least 64 KiB and a quarter of it are unused.
- **mmap placement.** `mmap` pages come from the lowest free run 16 KiB
  above `brk` (`/sys/module/rp2350_atrans/parameters/mmap_low`; 0 takes the
  highest run instead). That keeps the block's top free to shrink, and keeps
  a fork child small, since the child needs the block up to the highest
  mapped page.

### Where blocks come from

Blocks come from the ATRANS pool first ([memory.md](memory.md)): a stretch
of PSRAM that holds only ATRANS blocks, so it can be compacted. When the
pool can't satisfy a request, the driver takes a run from the page
allocator:

- **Why not `alloc_pages_exact()` alone:** it needs a free buddy block of
  the next power of two. A 2.1 MiB block needs an aligned, entirely free
  4 MiB, and 8 MiB of RAM has at most one such block.
  `alloc_contig_range()` needs compaction or CMA, and both need an MMU.
- **Runs of free buddy blocks.** The driver scans the zone for runs of
  adjacent free buddy blocks, under the zone lock, after draining the
  per-cpu lists. It takes the smallest run that fits (best fit), pulls those
  blocks off the free lists (`__isolate_free_page`, which also checks the
  watermark), turns them into ordinary order-0 pages and frees the overhang.
- **Fallback:** `alloc_pages_exact(GFP_KERNEL | __GFP_NORETRY)`, which can
  reclaim a little.
- **Then the page cache.** Clean page cache sits in free memory as scattered
  pages, so the runs can be missing even with MemAvailable high. Before
  failing, the driver drops the clean page cache and slab caches, as
  `drop_caches=3` does, and scans again (`cache drops` in `/proc/atrans`).
- **Then swap**, which swaps out the idle processes in the way
  ([atrans-swap.md](atrans-swap.md)). A request that still can't be met is
  `ENOMEM` for that call.

### Shared text

- **The text cache:** `struct atrans_text`, keyed by inode, size and mtime.
  A running binary can't be written (`ETXTBSY`), so the key is stable while
  any process uses it.
- The first exec allocates a contiguous block for the text, reads the file
  into it through its kernel address, and flushes the cache. Later execs and
  every `fork` take a reference. The last exit or exec frees the block.
- **Switching:** ATRANS7 is rewritten in the same critical section as
  ATRANS4, after the same flush, and only when the next process runs a
  different binary. It adds no flush of its own.
- **Write protection, user mode:** MPU region 2 covers [0x11c00000, text
  end), read-only and executable. A store is a MemManage fault, so SIGSEGV.
  A load past the text block faults too (ATRANS7's SIZE is the text size).
- **Write protection, kernel:** the kernel's privileged default map would
  still let it write through the window, so `put_user`, `copy_to_user`,
  `clear_user` and the futex writes refuse text-window addresses with
  `EFAULT` (`rp2350_atrans_ro()` in `asm/uaccess.h`).
- **Reads:** `access_ok` accepts text-window pointers, so syscalls can read
  string literals and constant tables (`write(1, "literal", n)`).
  `strnlen_user` and `strncpy_from_user` bound strings at the end of
  whichever window holds them (`rp2350_atrans_str_max`).
- **Remote reads** (`/proc/<pid>/cmdline`, `mem`) of text go through the
  text block's uncached alias.
- **Swap** writes out only the data block. The text stays resident while any
  process uses it.

For example, three Lua processes share one 188 KiB text block, and each
has a 136 KiB data block (64 KiB stack, data and heap), against about
330 KiB each with the v2 layout.

### fork copies only what is in use

`dup_mmap` (nommu) calls the driver. The child gets a block of
`used + 32 KiB` (at most the parent's size). The driver copies three things
from the cached window (the parent is current) to the child's uncached
alias:

- the stack from the user sp up to the image
- image and heap up to `brk`
- each run of mapped pages

If other tasks share the mm (threads, a vfork child), or sp is outside the
stack region, the whole stack region is copied instead. Memory the child is
not given a copy of (below sp, past `brk`) is not zeroed. Programs never see
it as fresh memory: `brk` growth and `mmap` zero their pages.

Copying runs at about 19 MB/s, so the cost of `fork` grows with the memory
in use: about 5 ms for a small process, and about 60 ms with 1 MiB of heap.

### Switching, cache rules, MPU

- **`switch_mm`:** when a different ATRANS process runs, the driver cleans
  and invalidates the XIP cache in one set/way pass, with the cleans issued
  at the top of the maintenance window ([qmi-atrans.md](qmi-atrans.md)),
  then rewrites ATRANS4 (and ATRANS7 if the binary differs). Switches to and
  from FDPIC processes or kernel threads leave the window alone.
- **`switch_mm` runs even when `prev == next`.** A kernel thread (kswapd, a
  workqueue) runs on the mm of the last user task (lazy TLB), so switching
  back to that task passes `prev == next`. Whatever the kernel thread did to
  the block meanwhile (a pool move, a cancelled swap-out that unloaded the
  window) must still be undone, or the task returns to user mode through a
  stale or empty window and the kernel's store of the exception frame on the
  user stack faults with interrupts off.
- **Cost:** about 130 µs of flush and ATRANS write per switch. Refilling the
  16 KiB XIP cache, which serves kernel text and kernel data too, brings a
  whole switch to about 650 µs: a pipe round trip between two ATRANS
  processes (two switches) takes about 1.3 ms, against 0.44 ms between two
  FDPIC processes.
- **Cache invariant:** lines tagged with window addresses only ever belong to
  the loaded block. So an unloaded block can be read and written through the
  uncached untranslated alias (0x1d000000 + device offset).
- **New blocks:** a newly allocated block, or in-place growth, may still have
  dirty lines under its kernel address from earlier kernel use. The cache is
  flushed before the block is written any other way.
- **MPU (PMSAv8) while an ATRANS process is current:**
  - Region 0 is [window + 32, block top), read-write.
  - Region 1 is the thread pointer granule, read-only.
  - Region 2 is the shared text, read-only and executable.
  - The MPU runs with `PRIVDEFENA`, so the kernel keeps the default map.
    Other processes run with the MPU off.
  - PMSAv8 has no "kernel read-write, user read-only" encoding, so the
    kernel turns the MPU off for the one store that updates the thread
    pointer word.
- **`access_ok`, `get_user`, `put_user`** accept only pointers inside the
  block, above the thread pointer word, or in the text window for reads.
  Zero-length accesses are allowed at any address, as on stock Linux (musl's
  stdio flush passes a `{NULL, 0}` iovec).
- **Uaccess macros evaluate their pointer once.** `get_user(len, p++)` (as
  in random.c's `RNDADDENTROPY`) must not skip a word.

### Clone, threads, futexes, TLS

- **`clone` without `CLONE_VM`** goes through `dup_mmap`, so an ATRANS
  process gets fork semantics. For a plain nommu process, stock Linux would
  give the child an empty mm while it kept running in its parent's memory,
  so that case returns `EINVAL`.
- **vfork and threads** are stock code. Threads share the mm, so no window
  switch happens between them.
- **Futex keys.** Stock nommu keys a private futex by address alone (one
  address space). ATRANS processes share window addresses, so a wake in one
  process could wake a waiter in another and lose the real wakeup. The keys
  include the mm for ATRANS processes (`kernel/futex/core.c`).
- **TLS:** stock v7-M keeps `tp_value` per thread, and `set_tls`/`get_tls`
  work. The fast-path word is updated through a `THREAD_NOTIFY_SWITCH`
  notifier.

### Signals, faults and bad stack pointers

- **User-mode faults become signals.** Stock ARMv7-M Linux sends MemManage,
  BusFault and UsageFault to `__invalid_entry`, which spins forever.
  `linux/mach-rp2350/fault.c` and `__fault_entry` turn them into signals.
- **Signal handlers** need no change: the sigreturn code goes on the user
  stack, and SA_SIGINFO fault recovery works.
- **Faults while stacking.** On exception entry, the core stacks the frame
  onto the user sp. If that faults (`MSTKERR` or `STKERR`: a stack overflow
  below 0x11000020, an sp in kernel RAM, an unmapped sp), no frame exists.
  `__fault_entry` points PSP at a harmless frame, the task gets a fatal
  SIGSEGV, and a pending SVC is dropped.
- **Why the harmless frame has to be readable.** Even with PendSV pending,
  the M33 pops the frame on exception return before tail-chaining. A frame
  in kernel memory would fault forever (`MUNSTKERR`) under the MPU. So for an
  ATRANS process the frame goes in the read-only thread pointer granule,
  which user mode can read.
- **The exception frame on return.** On every return to user mode, the
  kernel writes the frame below the user sp with privileged stores. So the
  sp restored by `sigreturn` (`valid_user_regs`) and clone's new stack
  (`copy_thread`) must lie inside the block; otherwise they get `SIGSEGV` or
  `EINVAL`.
- **A kernel fault hangs.** `__invalid_entry` prints the fault status and
  the windows before it stops.

### /proc

- **Another process's memory:** `access_remote_vm` (nommu) goes to the
  driver for ATRANS processes. It reads or writes through the window if the
  block is loaded, and through the uncached alias if not. So
  `/proc/<pid>/cmdline`, `environ` and `mem` work, and `ps` shows real
  command lines and VSZ (data plus text).
- **`/proc/<pid>/maps`** shows the stack, the image (with its path), the
  heap, the mapped runs, the block, and for v3 the text window and its text
  block with its number of users:

  ```
  11000000-11010000 rw-p 00000000 00:00 0 [stack]
  11010000-11015000 rw-p 00000000 00:00 0 /usr/bin/forktest3
  11025000-11400000 ---p 00000000 00:00 0 [block 0x72a000, 148 KiB]
  11c00000-11c02dc8 r-xs 00000000 00:00 0 /usr/bin/forktest3
  11c03000-12000000 ---p 00000000 00:00 0 [text 0x2d4000, 12 KiB, 2 users]
  ```
- **`/proc/atrans`** has the statistics (fork copies, growth, cache drops,
  the pool), the shared texts and a per-process list of blocks.
- **ptrace** is unsupported and fails cleanly.

### Core-kernel changes (`linux/apply.sh`)

`apply.sh` patches each file from a pristine copy (`FILE.rp2350-orig`).

| File | Change |
|---|---|
| `asm/mmu.h` | `atrans_kva`, `atrans_size`, `atrans` in the nommu `mm_context_t` |
| `asm/mmu_context.h` | nommu `switch_mm` calls the driver, even when `prev == next` |
| `asm/uaccess.h` | `access_ok`, `get_user`, `put_user`, `TASK_SIZE_MAX` check the block; text-window writes (`put_user`, `copy_to_user`, `clear_user`) get `EFAULT` |
| `asm/futex.h` | futex writes refuse the text window |
| `lib/strnlen_user.c`, `lib/strncpy_from_user.c` | the string limit is the end of the window that holds the string |
| `asm/ptrace.h` | v7-M `valid_user_regs` checks the sp of an ATRANS process |
| `kernel/entry-v7m.S` | MemManage, BusFault and UsageFault go to `__fault_entry`, which also handles stacking faults |
| `kernel/traps.c` | `set_tls` updates the thread pointer word |
| `kernel/process.c` | `copy_thread` refuses a clone stack outside the block |
| `kernel/fork.c` | nommu `sys_fork` for ATRANS processes; nommu `dup_mmap` calls the driver |
| `mm/nommu.c` | `brk`, `do_mmap`, `do_munmap`, `exit_mmap`, `__access_remote_vm` go to the driver |
| `kernel/futex/core.c` | private futex keys include the mm for ATRANS processes |
| `fs/proc/task_nommu.c` | `/proc/<pid>/maps` and VSZ for ATRANS processes |
| `kernel/sched/core.c` | the swap-in hook ([atrans-swap.md](atrans-swap.md)) |

`CONFIG_COMPAT_32BIT_TIME=y` is set because musl calls the 32-bit-time
syscalls (`nanosleep`, `futex`, `clock_nanosleep`, `ppoll`, ...) whenever
the value fits in 32 bits, and the `*_time64` ones only otherwise.

## Limitations

- **4 MiB of data per process**, the size of the window, in one physically
  contiguous block. The whole stack region is resident in every process.
- **No `mprotect`, no writable `MAP_SHARED`, no `dlopen`.** `prot` is not
  enforced inside the block.
- **The OOM killer can't weigh ATRANS processes:** nommu keeps no RSS
  counters, so per-process RSS reads 0.
- **ptrace and DMA into user buffers** are not supported. DMA through the
  window would follow whichever process is current.
- **One CPU.** Core 1 runs the I/O. SMP would need one window per core,
  which ATRANS can't provide: the translation and the XIP cache are shared by
  both cores.
- **A lazily grown stack** (an ATRANS4 BASE offset plus fault-driven growth)
  is possible but not built.

## Tests

`forktest` (v2 layout, `tests/atrans.ld`) and `forktest3` (the same source,
`tests/atrans-v3.ld`) are freestanding programs in the image. They cover
layout, auxv, fork, clone, vfork, threads, TLS, futexes, signals, mmap,
growth, `/proc`, the MPU, `EFAULT`, bad stack pointers, shared text and
out-of-memory, with timings.

```sh
forktest                      # every test
forktest grow threads badsp   # single tests
forktest sleep 3              # leaves a process to look at with ps
forktest3 text                # the shared-text checks
cat /proc/atrans
```

The ATRANS toolchain's musl self-test (`atrans/tests/selftest.c`) covers
pthreads, `posix_spawn`, ptys and 64-bit atomics.
