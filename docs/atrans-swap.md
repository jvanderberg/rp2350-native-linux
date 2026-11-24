# Swapping ATRANS processes

nommu Linux has no swap: `CONFIG_SWAP` needs page tables. But an ATRANS
process ([atrans.md](atrans.md)) lives in one PSRAM block, and its window
addresses never change. So the whole block can go to disk while the process
sleeps and come back at another physical address when it wakes, the way
classic Unix swapped whole processes.

`linux/drivers/rp2350/rp2350-atrans-swap.c` does this. The rest of the
kernel is untouched apart from one hook at the end of `schedule()`. On the
board, the swap device is the SD card's second partition (`/dev/mmcblk0p2`),
or a swap file on its FAT.

## Using it

```sh
atrans-mkswap /dev/mmcblk0p2          # once: a Linux swap signature (busybox has no mkswap on nommu)
echo /dev/mmcblk0p2 > /proc/atrans-swap
cat /proc/atrans-swap                 # statistics and the swapped-out processes
echo out 123 > /proc/atrans-swap      # swap out process 123 now (EBUSY if it isn't idle)
echo off > /proc/atrans-swap          # swap everything back in, then stop (EBUSY if it doesn't fit)
echo off force > /proc/atrans-swap    # the same, but kill what doesn't fit
echo low 1536 > /proc/atrans-swap     # background swap-out below 1536 KiB free (0: off)
echo age 10 > /proc/atrans-swap       # ...of processes asleep at least 10 s
```

- **Swap space** is a block device or a regular file with a Linux swap
  signature, the same format as `mkswap`, so a partition made with `mkswap`
  on a PC works. Page 0 is the header, and slots are pages 1 to `last_page`.
- **A file** (for example `/mnt/sd/atrans.swp` on the SD card's FAT) is
  mapped to device sectors once, with `bmap`, as `swapon` does. After that
  the I/O goes straight to the device, bypassing the filesystem. The file is
  marked `S_SWAPFILE`, so it can't be written or truncated while in use. It
  must have no holes. Make it with
  `dd if=/dev/zero of=/mnt/sd/atrans.swp bs=64k count=1024` (64 MiB) and
  `atrans-mkswap /mnt/sd/atrans.swp`. Don't use `bs=1M`: dd then needs a
  1 MiB buffer in one piece, which fragmented memory rarely has.
- **At boot,** `/etc/init.d/S04atransswap` runs `atrans-swapon`, and so does
  `S03sd` when a card goes in. `atrans-swapon` takes the first block device
  with a swap signature, or else `/mnt/sd/atrans.swp`, and does nothing if
  swap is already on. `S04atransswap` then sets `age 10` and `low 1536`:
  with less than 1.5 MiB free (free pages plus the pool's free space), a
  process asleep for 10 s goes to the card, one a second. When the card
  comes out, `S03sd` writes `off`.
- **Without `low`,** processes are swapped out only when a block can't be
  found, or when the kernel side is short of memory (below).

`free` and `top` show it as swap (`SwapTotal`, `SwapFree`; `/proc/swaps`
lists the device or file). A swapped-out process keeps its VSZ in `ps`; its
RSS drops to its shared text, which stays resident, and `VmSwap` in
`/proc/<pid>/status` is what swap holds of it ([memory.md](memory.md#what-free-top-and-ps-report)).
`/proc/<pid>/maps` shows `[swapped out]`. Killing it with SIGKILL gives its
slots back.

## How it works

### When, and who

- **On demand.** `block_alloc()` in rp2350-atrans.c serves exec, fork,
  growth and swap-in. When the pool, the page allocator and dropping the
  clean caches all fail, it calls `rp2350_atrans_swap_reclaim()`, which
  swaps out the processes in the way (next section), and retries while that
  succeeds. A waiting exec, fork or growth gives up with `ENOMEM` after
  about 3 s.
- **For the kernel side.** When a kernel or FDPIC allocation is short of
  memory and the ATRANS pool has no free space to lend, the pool's shrinker
  swaps out the process asleep longest, so its room can be lent
  ([memory.md](memory.md)). An idle session goes to the card instead of the
  OOM killer taking something.
- **In the background,** below the `low` threshold (above).
- **Victims** are ATRANS processes none of whose tasks is current or inside
  swap code. Each of their tasks must be off the CPU in a way that resumes
  through the swap-in hook: blocked, stopped, or preempted. A task that has
  never run starts in `ret_from_fork` instead, so its process is ruled out.
- **Order:** stopped processes first, then blocked ones, then runnable ones
  that weren't swapped in during the last second (so runnable processes
  don't just take turns). Within each group, the one that ran least
  recently (`se.exec_start`) goes first.
- **Never chosen:** a process in the middle of fork, exec or growth, or
  swapping itself in. A task waiting for swap carries `TIF_ATRANS_SWAP`, and
  growth holds the mmap lock, which the swapper only ever trylocks. A
  process blocked while it holds its own mmap lock is skipped for the same
  reason.
- **Never chosen either:** a process with a task inside a bio submission
  (`current->bio_list` set), or holding a block plug. One example is a task
  preempted at the `cond_resched()` in `mtd_queue_rq` while reading the
  root filesystem. Its swap-in would run from there, and the block layer
  would only queue the swap reads on that task's `bio_list`, behind the
  submission it hasn't finished. They would never be issued, and the task
  would wait on them for ever while holding `swap_lock`, so every other
  swap-in, swap-out and pool lend would block behind it. As a safety net,
  the hook kills a process that tries to swap in from inside a submission
  anyway, with a warning, instead of hanging.
- **Not retried at once:** a process whose swap-out was just cancelled (it
  woke during the write) isn't tried again by the shrinker or the background
  worker for 2 s. It is awake, so a second write would be cancelled too.

### Making room for a contiguous block

What `block_alloc` needs is one physically contiguous run, not just free
memory. So the swapper doesn't simply evict the oldest sleeper:

1. It classifies every page of the zone as free (a free buddy block), part
   of an idle ATRANS process's block (a candidate), or pinned. Pinned means
   kernel memory, page cache, FDPIC programs, shared text, or running
   processes.
2. It slides a window of the requested size over the zone, keeps only
   windows without a pinned page, and picks the one whose candidates have
   the fewest pages between them.
3. It also prices the pool's way: compaction makes any free space in the
   pool usable, so swapping out pool blocks until the pool has enough free
   space in all also works. The cheaper of the two wins.
4. It swaps exactly those candidates out (`targeted` in
   `/proc/atrans-swap`).

If neither way exists, swapping can't help and `block_alloc` gets ENOMEM at
once, instead of evicting everything in turn. If the window is already free
but a watermark refused it, the process that ran least recently goes
instead, to free memory elsewhere.

### Swap-out

1. Take the mmap lock (trylock) and check again that the process is idle.
2. Collect what it uses, by the same rule as fork: the stack from the page
   holding sp (all of it if several tasks share the mm), the image up to
   `brk`, and the mapped runs. Shared text stays resident.
3. Give it one contiguous run of slots if there is one, so the SD card gets
   long sequential writes. Otherwise it gets one run per range.
4. Set the state to OUT. Write back the window's cache lines, but leave the
   window pointing at the block (see "Races and locks"). Write the pages with
   bios over the block's own `struct page`s, up to 1 MiB per bio, all in
   flight at once.
5. Clean and invalidate the XIP cache. If nothing cancelled the swap-out,
   set `atrans_kva = atrans_size = 0`, empty the window if it shows the
   block, and free the block.

### Swap-in: the `schedule()` hook

With `PREEMPT_NONE`, a task off the CPU resumes in one of two ways:

- by returning from `schedule()`: it blocked, stopped, or was preempted on
  its way back to user mode;
- by returning from `preempt_schedule_common()`: a `cond_resched()`.

The one exception is a new task, which starts in `ret_from_fork`. Between
the context switch and either return, nothing touches user memory. So
`kernel/sched/core.c` (patched by `apply.sh`) calls
`rp2350_atrans_swap_resume()` at the end of both. If the process is swapped
out, that function:

1. allocates a block (which may swap others out),
2. reads the pages back,
3. cleans the cache,
4. calls `set_block()` and `ensure_loaded()`,

all in the task's own context, where it may sleep. Then it returns to
whatever the task was doing: a `read()`, `nanosleep`, `wait4`, a stop. The
syscall's user access that follows finds its memory where it always was.

`switch_mm` to a swapped-out process empties the window, so in the short gap
before the hook the previous process's block isn't exposed.

**When there is no room** even after `block_alloc` has swapped out every
idle process in the way, the task waits in 100 ms steps, with `swap_lock`
released, for others to exit or go idle. It logs one line when it starts
waiting:

```
rp2350-atrans-swap: swaptest[151] waits for a 512 KiB block (1748 KiB free)
```

After 30 steps (about 3 s), or 3 once a fatal signal is pending, it is
killed instead, with a log line, and exits without its memory:

```
rp2350-atrans-swap: swaptest[164] (336 KiB in swap) killed: no room to swap it back in (1688 KiB free, swap 500 of 508 KiB used)
```

It isn't left waiting forever because it may hold locks other processes
need: whatever it was sleeping in when it was swapped out, it still is. A
read error kills it at once the same way (`swap read error`).

**The window check.** The hook also reloads current's window whenever the
window doesn't show current's block (`window reloads` in
`/proc/atrans-swap`). `switch_mm` already repoints the window when a task
comes back to an mm a kernel thread borrowed ([atrans.md](atrans.md)); the
check is a second line of defence against a block moved by the pool's
shrinker (from kswapd) or unloaded by a cancelled background swap-out.

### Races and locks

- **A victim that wakes during its own swap-out doesn't wait.** The hook
  sees state OUT, marks it CANCEL and goes on running in its block, which is
  still intact. The swapper throws its copy away when the I/O ends. The
  victim might hold a lock the I/O needs, so waiting could deadlock. This is
  why the window stays mapped during the I/O.
- **`swap_lock` (a mutex) serialises all swapping and the slot map.** It can
  be taken while an mmap lock is held (growth, `/proc` readers), so code
  holding it only ever trylocks mmap locks.
- **The kernel side waits for the swap lock, briefly.** A swap-in or
  swap-out holds it across its card I/O. The pool's shrinker, swapping out
  for a waiting allocation, polls for it for up to 400 ms rather than giving
  up at once, and then only trylocks for a second, so one stall stays one
  stall and it can't deadlock on a lock it holds itself.
- **`TIF_ATRANS_SWAP` marks a task inside swap code.** The hook ignores the
  sleeps such a task makes, so nothing recurses, and its process is never a
  victim.
- **The SD driver** (`rp2350-sd.c`) suits this: blk-mq, completion in the
  IRQ, and nothing held across the wait.
- **A written block's CRC is of the bytes that went out.** A cancelled
  victim runs and writes its memory while the I/O is still sending it, so
  core 1 computes each block's CRC16 as it sends it
  ([boot-and-io.md](boot-and-io.md)). The swapper discards that copy anyway;
  the CRC just has to match, or the card refuses the block.

### Kernel access to a swapped-out process

- `/proc/<pid>/cmdline`, `environ` and `mem` reads (`access_remote_vm`) read
  the pages straight from swap, so `ps` doesn't bring processes back in.
  Writes fail.
- `access_ok()` fails for a swapped-out process: its block size is 0.
- Signals are delivered by the process itself after the hook has run.
- **When swap-in is impossible** (a read error, or no room after about 3 s),
  the process is marked DEAD, its slots are freed, and every task of it gets
  SIGKILL, with a log line. It exits without its memory, and every user
  access on the exit path fails `access_ok` cleanly. It never runs on
  garbage.

### swapoff and card removal

`echo off` first swaps every swapped-out process back in (`swapped in by
swapoff` in `/proc/atrans-swap`). No new swap-outs start meanwhile. swapoff
can't load another process's window, so a process brought back this way is
in state LOAD, and its own hook loads its window when it next runs. Then:

- **Everything is back in:** swap stops, and the file or device is released.
- **Some don't fit:** swap stays on, those processes stay safely in swap,
  and the write fails with EBUSY:

  ```
  rp2350-atrans-swap: swapoff refused: 1 swapped-out processes don't fit back in memory ("off force" kills them)
  ```

- **`off force`** kills the ones that don't fit, with a log line each, and
  stops:

  ```
  rp2350-atrans-swap: swaptest[123] (336 KiB in swap) killed: swapoff force, and no room for it (1752 KiB free, swap 336 of 508 KiB used)
  ```

- **A process whose data can't be read back** is killed in every case
  (`swapoff, and its data can't be read back`).

**Card removal.** When the card comes out, `S03sd` writes `off`. The SD
driver has set the disk's capacity to 0 by then, so swapoff treats it as
`off force`. Every read then fails, so each swapped-out process is killed
with `swapoff, and its data can't be read back` (or `swapoff with the device
gone, and no room for it` if memory is short). A swapped-out process that
wakes before the `off` arrives gets the read error itself and is killed
(`swap read error`). Either way, nothing comes back with garbage.

### Cache rules

The swap I/O goes through the block's kernel address, which nothing else
uses for a live block. Window lines are written back before a swap-out reads
the block. After any swap I/O the whole XIP cache is cleaned and
invalidated, so no kernel-address lines for a process block outlive the
I/O. That keeps the cache invariant in [atrans.md](atrans.md).

## Cost

On the SD card, swap-out and swap-in each run at about 3.1 MB/s, so about
330 ms per MiB ([performance.md](performance.md)). A process holding 256 KiB
comes back in well under a second.

A PSRAM-backed device works too, for testing without a card: a DTB with
`memory` at 6 MiB instead of 8, `phram` over the top 2 MiB
(`echo swap,0x11a00000,0x200000 > /sys/module/phram/parameters/phram`) and
`mtdblock` on top. It needs `CONFIG_MTD_PHRAM=y` and a read-write
`CONFIG_MTD_BLOCK=y`, which the committed fragments don't set. It runs at
about 6.9 MB/s each way.

## Testing

`swaptest` (`tests/swaptest.c`, in the image):

```sh
swaptest [N [KIB [ROUNDS]]]   # default 4 children, 512 KiB, 3 rounds
swaptest hold KIB SECONDS     # hold KIB KiB of pattern, sleep, check
swaptest wake KIB SECONDS MS  # hold it, waking every MS ms
swaptest pipes KIB            # fill pipes nobody reads
```

- `swaptest N KIB ROUNDS` forks N children. Each grows its heap by KIB KiB,
  maps 64 KiB and puts 8 KiB of pattern on its stack, fills all of that with
  a pattern of its own, and sleeps in `read()`.
- The parent wakes the children in turn. Each child checks every word,
  writes the next generation and goes back to sleep. The parent also reads
  `/proc/<pid>/cmdline` of the sleeping children.
- `swaptest wake` exercises a swap-out cancelled by its victim waking, and
  the window reload after it.

## Limitations

- One swap area at a time, and no priorities.
- A process goes whole or not at all: no swap-out of a single thread's
  stack.
- FDPIC processes (PID 1 and the small daemons) can't be swapped: they live
  in ordinary kernel memory, scattered.
- Pulling the card while processes are swapped out is handled by the code
  paths for capacity 0 and read errors; it has not been exercised on the
  board.
