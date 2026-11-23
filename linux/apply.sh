#!/usr/bin/env bash
# Add the RP2350 machine and drivers to a stock Linux tree (re-runnable).
# usage: apply.sh KERNEL_TREE
set -euo pipefail
K=$1
H=$(dirname "$0")
cd "$K"
rm -rf arch/arm/mach-rp2350 drivers/rp2350
cp -r "$H/mach-rp2350" arch/arm/mach-rp2350
cp -r "$H/drivers/rp2350" drivers/rp2350
# prototypes of the ATRANS hooks (copied only when changed: it is widely included)
cmp -s "$H/include/asm/rp2350-atrans.h" arch/arm/include/asm/rp2350-atrans.h ||
  cp "$H/include/asm/rp2350-atrans.h" arch/arm/include/asm/rp2350-atrans.h

# Core files are patched from a pristine copy (FILE.rp2350-orig, saved on
# the first run) into FILE.rp2350-new, which replaces FILE only if it
# differs: an edited patch takes effect, an unchanged file keeps its mtime.
FILES="mm/slub.c mm/slab.h drivers/mtd/maps/physmap-core.c arch/arm/kernel/vmlinux-xip.lds.S arch/arm/mm/init.c arch/arm/Kconfig arch/arm/Makefile drivers/Kconfig drivers/Makefile arch/arm/include/asm/mmu.h arch/arm/include/asm/mmu_context.h arch/arm/include/asm/uaccess.h arch/arm/kernel/entry-v7m.S kernel/fork.c mm/nommu.c arch/arm/kernel/traps.c arch/arm/kernel/process.c arch/arm/include/asm/ptrace.h kernel/futex/core.c fs/proc/task_nommu.c arch/arm/include/asm/futex.h lib/strnlen_user.c lib/strncpy_from_user.c"
# rp2350-atrans-swap.c: the schedule() hook
FILES="$FILES kernel/sched/core.c"
# memory: iomap's bioset reserve
FILES="$FILES fs/iomap/buffered-io.c fs/squashfs/squashfs_fs.h"
# memory: tmpfs (ramfs) size=, tty buffers, kernel stacks
FILES="$FILES fs/ramfs/inode.c fs/ramfs/file-nommu.c fs/ramfs/internal.h include/linux/tty.h drivers/tty/tty_buffer.c include/linux/thread_info.h"
# memory: the OOM killer's sysctls on nommu, protected daemons
FILES="$FILES mm/oom_kill.c fs/exec.c"
# memory reporting: free memory and swap with the ATRANS pool and swap
FILES="$FILES mm/show_mem.c include/linux/swap.h"
for f in $FILES; do
  if [ ! -f "$f.rp2350-orig" ]; then
    if grep -qi rp2350 "$f"; then
      echo "apply.sh: $f is patched but has no $f.rp2350-orig; re-extract the tree" >&2
      exit 1
    fi
    cp -p "$f" "$f.rp2350-orig"
  fi
  cp "$f.rp2350-orig" "$f.rp2350-new"
done
N=.rp2350-new
sed -i 's/^config ARCH_MPS2$/config ARCH_RP2350\n\tbool "Raspberry Pi RP2350 (Cortex-M33)"\n\tdepends on ARM_SINGLE_ARMV7M\n\nconfig ARCH_MPS2/' arch/arm/Kconfig$N
sed -i 's/^machine-$(CONFIG_ARCH_STM32)\t\t+= stm32$/&\nmachine-$(CONFIG_ARCH_RP2350)\t\t+= rp2350/' arch/arm/Makefile$N
sed -i '$i source "drivers/rp2350/Kconfig"' drivers/Kconfig$N
echo 'obj-y += rp2350/' >> drivers/Makefile$N

# ---- per-process ATRANS address spaces (CONFIG_RP2350_ATRANS) ----
# mm_context_t (nommu): the process's PSRAM block
perl -0pi -e 's/(typedef struct \{\n\tunsigned long\tend_brk;\n)/$1#ifdef CONFIG_RP2350_ATRANS\n\tunsigned long\tatrans_kva;\t\/* 0: not an ATRANS process *\/\n\tunsigned long\tatrans_size;\n\tstruct rp2350_atrans_mm *atrans;\t\/* the rest (rp2350-atrans.c) *\/\n#endif\n/' arch/arm/include/asm/mmu.h$N
# switch_mm (nommu): repoint the window, even when prev == next. A kernel thread
# (kswapd, the swap worker) runs on the mm of the last user task, and
# switching back to that task passes prev == next; whatever the kernel
# thread did to its window meanwhile (swap-out unloads ATRANS4 before the
# I/O, the pool moves blocks) has to be redone (a HardFault in
# no_work_pending writing the exception frame through an empty window).
perl -0pi -e 's/(static inline void\nswitch_mm\(struct mm_struct \*prev)/#ifdef CONFIG_RP2350_ATRANS\nvoid rp2350_atrans_switch(struct mm_struct *next);\n#endif\n\n$1/; s/(\t\tif \(cache_is_vivt\(\)\)\n\t\t\tcpumask_clear_cpu\(cpu, mm_cpumask\(prev\)\);\n\t\}\n)#endif\n\}/$1#elif defined(CONFIG_RP2350_ATRANS)\n\trp2350_atrans_switch(next);\n#endif\n}/' arch/arm/include/asm/mmu_context.h$N
# fork() for ATRANS processes; their block is copied by dup_mmap (nommu)
perl -0pi -e 's/\t\/\* can not support in nommu mode \*\/\n\treturn -EINVAL;/#ifdef CONFIG_RP2350_ATRANS\n\tif (current->mm && current->mm->context.atrans_kva) {\n\t\tstruct kernel_clone_args args = { .exit_signal = SIGCHLD };\n\n\t\treturn kernel_clone(&args);\n\t}\n#endif\n\t\/* can not support in nommu mode *\/\n\treturn -EINVAL;/; s/(static int dup_mmap\(struct mm_struct \*mm, struct mm_struct \*oldmm\)\n\{\n\tmmap_write_lock\(oldmm\);\n\tdup_mm_exe_file\(mm, oldmm\);\n\tmmap_write_unlock\(oldmm\);\n)\treturn 0;\n\}/#ifdef CONFIG_RP2350_ATRANS\nint rp2350_atrans_dup(struct mm_struct *oldmm, struct mm_struct *mm);\n#endif\n$1#ifdef CONFIG_RP2350_ATRANS\n\treturn rp2350_atrans_dup(oldmm, mm);\n#else\n\treturn 0;\n#endif\n}/' kernel/fork.c$N
# exit_mmap (nommu): free the block
perl -0pi -e 's/(void exit_mmap\(struct mm_struct \*mm\)\n\{)/#ifdef CONFIG_RP2350_ATRANS\nvoid rp2350_atrans_exit(struct mm_struct *mm);\n#endif\n\n$1/; s/(\tmm->total_vm = 0;\n)/$1#ifdef CONFIG_RP2350_ATRANS\n\trp2350_atrans_exit(mm);\n#endif\n/' mm/nommu.c$N
# mmap/munmap/brk (nommu) for ATRANS processes: pages inside the block,
# which grows; /proc/<pid>/cmdline etc. (access_remote_vm) read the block
perl -0pi -e 's/(unsigned long do_mmap\(struct file \*file,\n)/#ifdef CONFIG_RP2350_ATRANS\nunsigned long rp2350_atrans_mmap(struct file *file, unsigned long addr, unsigned long len,\n\t\t\t\t unsigned long prot, unsigned long flags, unsigned long pgoff);\nint rp2350_atrans_munmap(struct mm_struct *mm, unsigned long start, size_t len);\n#endif\n\n$1/; s/(\n\t\*populate = 0;\n)/$1#ifdef CONFIG_RP2350_ATRANS\n\tif (current->mm && current->mm->context.atrans_kva)\n\t\treturn rp2350_atrans_mmap(file, addr, len, prot, flags, pgoff);\n#endif\n/; s/(int do_munmap\(struct mm_struct \*mm, unsigned long start, size_t len, struct list_head \*uf\)\n\{\n\tVMA_ITERATOR\(vmi, mm, start\);\n\tstruct vm_area_struct \*vma;\n\tunsigned long end;\n\tint ret = 0;\n)/$1\n#ifdef CONFIG_RP2350_ATRANS\n\tif (mm->context.atrans_kva)\n\t\treturn rp2350_atrans_munmap(mm, start, len);\n#endif\n/; s/(SYSCALL_DEFINE1\(brk, unsigned long, brk\)\n\{\n\tstruct mm_struct \*mm = current->mm;\n\n)/#ifdef CONFIG_RP2350_ATRANS\n#include <asm\/rp2350-atrans.h>\n#endif\n$1#ifdef CONFIG_RP2350_ATRANS\n\tif (mm->context.atrans_kva)\n\t\treturn rp2350_atrans_brk(mm, brk);\n#endif\n/; s/(static int __access_remote_vm\(struct mm_struct \*mm, unsigned long addr,\n\t\t\t      void \*buf, int len, unsigned int gup_flags\)\n\{\n\tstruct vm_area_struct \*vma;\n\tint write = gup_flags & FOLL_WRITE;\n\n)/$1#ifdef CONFIG_RP2350_ATRANS\n\tif (mm->context.atrans)\n\t\treturn rp2350_atrans_access(mm, addr, buf, len, write);\n#endif\n/' mm/nommu.c$N
# ---- CONFIG_RP2350_MEMSTAT: /proc/memstat, /proc/memmap (mm/rp2350-memstat.inc) ----
# slab pages per cache (SLUB_TINY keeps no count of full slabs), and the
# live large kmallocs (over 8 KiB: page allocations counted as SUnreclaim)
# by caller
cmp -s "$H/mm/rp2350-memstat.inc" mm/rp2350-memstat.inc || cp "$H/mm/rp2350-memstat.inc" mm/rp2350-memstat.inc
perl -0pi -e 's/(\tconst char \*name;\t\t\/\* Name \(only for display!\) \*\/\n)/$1#ifdef CONFIG_RP2350_MEMSTAT\n\tatomic_long_t rp_pages;\n#endif\n/' mm/slab.h$N
perl -0pi -e 's/(\tmod_node_page_state\(slab_pgdat\(slab\), cache_vmstat_idx\(s\),\n\t\t\t    PAGE_SIZE << order\);\n)/$1#ifdef CONFIG_RP2350_MEMSTAT\n\tatomic_long_add(1L << order, &s->rp_pages);\n#endif\n/; s/(\tmod_node_page_state\(slab_pgdat\(slab\), cache_vmstat_idx\(s\),\n\t\t\t    -\(PAGE_SIZE << order\)\);\n)/$1#ifdef CONFIG_RP2350_MEMSTAT\n\tatomic_long_sub(1L << order, &s->rp_pages);\n#endif\n/; s/(static void \*___kmalloc_large_node\(size_t size, gfp_t flags, int node\)\n)/#ifdef CONFIG_RP2350_MEMSTAT\nvoid rp2350_memstat_large(void *p, size_t size, unsigned long caller);\nvoid rp2350_memstat_large_free(void *p);\n#else\n#define rp2350_memstat_large(p, s, c) do { } while (0)\n#define rp2350_memstat_large_free(p) do { } while (0)\n#endif\n$1/; s/(\ttrace_kmalloc\(_RET_IP_, ret, size, PAGE_SIZE << get_order\(size\),\n\t\t      flags, NUMA_NO_NODE\);\n)/$1\trp2350_memstat_large(ret, size, _RET_IP_);\n/; s/(\ttrace_kmalloc\(_RET_IP_, ret, size, PAGE_SIZE << get_order\(size\),\n\t\t      flags, node\);\n)/$1\trp2350_memstat_large(ret, size, _RET_IP_);\n/; s/(\t\ttrace_kmalloc\(caller, ret, size,\n\t\t\t      PAGE_SIZE << get_order\(size\), flags, node\);\n)/$1\t\trp2350_memstat_large(ret, size, caller);\n/; s/(\tkmemleak_free\(object\);\n\tkasan_kfree_large\(object\);\n)/$1\trp2350_memstat_large_free(object);\n/' mm/slub.c$N
printf '\n#ifdef CONFIG_RP2350_MEMSTAT\n#include "rp2350-memstat.inc"\n#endif\n' >> mm/slub.c$N
for f in mm/slab.h:'atomic_long_t rp_pages;' mm/slub.c:'atomic_long_add(1L << order' mm/slub.c:'atomic_long_sub(1L << order' mm/slub.c:'rp2350_memstat_large(ret, size, caller);' mm/slub.c:'rp2350_memstat_large_free(object);' mm/slub.c:'#include "rp2350-memstat.inc"'; do
  grep -qF "${f#*:}" "${f%%:*}$N" || { echo "apply.sh: patching ${f%%:*} failed" >&2; exit 1; }
done
[ "$(grep -c 'rp2350_memstat_large(ret, size, _RET_IP_);' mm/slub.c$N)" = 2 ] || { echo "apply.sh: patching mm/slub.c (large) failed" >&2; exit 1; }
# ---- whole-process swapping (CONFIG_RP2350_ATRANS_SWAP, rp2350-atrans-swap.c) ----
# a task that left the CPU resumes by returning from schedule() (blocked,
# or preempted on the way to user mode) or, with PREEMPT_NONE, from
# preempt_schedule_common() (cond_resched): swap it back in first
perl -0pi -e 's/(#include "\.\.\/workqueue_internal\.h"\n)/$1#ifdef CONFIG_RP2350_ATRANS_SWAP\n#include <asm\/rp2350-atrans.h>\n#endif\n/; s/(\t__schedule_loop\(SM_NONE\);\n\tsched_update_worker\(tsk\);\n)/$1#ifdef CONFIG_RP2350_ATRANS_SWAP\n\trp2350_atrans_swap_resume();\n#endif\n/; s/(\t\t \* between schedule and now\.\n\t\t \*\/\n\t\} while \(need_resched\(\)\);\n)/$1#ifdef CONFIG_RP2350_ATRANS_SWAP\n\trp2350_atrans_swap_resume();\t\/* cond_resched *\/\n#endif\n/' kernel/sched/core.c$N
for f in kernel/sched/core.c:'rp2350_atrans_swap_resume();' kernel/sched/core.c:'rp2350_atrans_swap_resume();	/* cond_resched */' kernel/sched/core.c:'#include <asm/rp2350-atrans.h>'; do
  grep -qF "${f#*:}" "${f%%:*}$N" || { echo "apply.sh: patching ${f%%:*} failed" >&2; exit 1; }
done

# ---- memory ----
# iomap's writeback bioset keeps 32 bios with 256-entry bvecs in reserve
# (biovec-max, 3 KiB each, two to an 8 KiB slab: 128 KiB). Nothing here
# writes back through iomap (vfat and block devices use buffer heads,
# jffs2 the MTD), so keep the minimum, BIO_POOL_SIZE.
perl -0pi -e 's/(return bioset_init\(&iomap_ioend_bioset, )4 \* \(PAGE_SIZE \/ SECTOR_SIZE\),/$1BIO_POOL_SIZE,\t\/* rp2350: was 4 * (PAGE_SIZE \/ SECTOR_SIZE) *\//' fs/iomap/buffered-io.c$N
grep -qF 'BIO_POOL_SIZE,	/* rp2350' fs/iomap/buffered-io.c$N || { echo "apply.sh: patching fs/iomap/buffered-io.c failed" >&2; exit 1; }
# squashfs caches 8 decompressed metadata blocks (8 KiB each, as kmalloc-4k
# pages); 4 do for this root filesystem
perl -0pi -e 's/(#define SQUASHFS_CACHED_BLKS\t\t)8\n/${1}4\t\/* rp2350: was 8 *\/\n/' fs/squashfs/squashfs_fs.h$N
grep -qF '4	/* rp2350: was 8 */' fs/squashfs/squashfs_fs.h$N || { echo "apply.sh: patching fs/squashfs/squashfs_fs.h failed" >&2; exit 1; }
# A private file mapping (an FDPIC program's text and data from the
# squashfs) is a copy: drop the clean page cache its read left. Those pages
# would sit scattered in free memory, splitting the runs that the next
# copy, kernel stack or ATRANS block needs (nommu can't compact).
perl -0pi -e 's/(\t\t\/\* clear the last little bit \*\/\n\t\tif \(ret < len\)\n\t\t\tmemset\(base \+ ret, 0, len - ret\);\n)/$1#ifdef CONFIG_ARCH_RP2350\n\t\tinvalidate_mapping_pages(vma->vm_file->f_mapping, vma->vm_pgoff,\n\t\t\t\t\t vma->vm_pgoff + ((len - 1) >> PAGE_SHIFT));\t\/* rp2350 *\/\n#endif\n/' mm/nommu.c$N
grep -qF 'PAGE_SHIFT));	/* rp2350 */' mm/nommu.c$N || { echo "apply.sh: patching mm/nommu.c (page cache) failed" >&2; exit 1; }
# The copy itself: alloc_pages_exact wants a free power-of-two block and
# runs the OOM killer for orders up to 3 when scattered pages deny one.
# rp2350_alloc_exact tries an exact run, the ATRANS pool's free space and
# a cache drop first (drivers/rp2350/rp2350-atrans.c).
perl -0pi -e 's/(\tbase = )alloc_pages_exact\(total << PAGE_SHIFT, GFP_KERNEL\);\n(\tif \(!base\)\n\t\tgoto enomem;)/#ifdef CONFIG_RP2350_ATRANS\n${1}rp2350_alloc_exact(total << PAGE_SHIFT);\t\/* rp2350 *\/\n#else\n${1}alloc_pages_exact(total << PAGE_SHIFT, GFP_KERNEL);\n#endif\n$2/' mm/nommu.c$N
grep -qF 'rp2350_alloc_exact(total << PAGE_SHIFT);	/* rp2350 */' mm/nommu.c$N || { echo "apply.sh: patching mm/nommu.c (alloc) failed" >&2; exit 1; }
# tmpfs is ramfs here (CONFIG_SHMEM needs an MMU), and ramfs ignores size=:
# a big file in /tmp would eat kernel memory until the OOM killer ran. Keep size=
# (ENOSPC past it, fs/ramfs/rp2350-size.inc), let ramfs page allocations
# fail with ENOMEM instead of running the OOM killer, and show size in df.
cmp -s "$H/fs/ramfs/rp2350-size.inc" fs/ramfs/rp2350-size.inc || cp "$H/fs/ramfs/rp2350-size.inc" fs/ramfs/rp2350-size.inc
perl -0pi -e 's/(struct ramfs_mount_opts \{\n\tumode_t mode;\n)/$1#ifdef CONFIG_ARCH_RP2350\n\tunsigned long max_pages;\n#endif\n/; s/(#define RAMFS_DEFAULT_MODE\t0755\n)/$1\n#ifdef CONFIG_ARCH_RP2350\n#include "rp2350-size.inc"\n#define RAMFS_STATFS ramfs_rp_statfs\n#else\n#define RAMFS_STATFS simple_statfs\n#endif\n/; s/\t\tinode->i_mapping->a_ops = &ram_aops;\n\t\tmapping_set_gfp_mask\(inode->i_mapping, GFP_HIGHUSER\);\n/#ifdef CONFIG_ARCH_RP2350\n\t\tinode->i_mapping->a_ops = ramfs_rp_get_aops();\n\t\tmapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER | __GFP_RETRY_MAYFAIL | __GFP_NOWARN);\n#else\n$&#endif\n/; s/(static const struct super_operations ramfs_ops = \{\n\t\.statfs\t\t= )simple_statfs,/${1}RAMFS_STATFS,/; s/(\t\tseq_printf\(m, ",mode=%o", fsi->mount_opts\.mode\);\n)/$1#ifdef CONFIG_ARCH_RP2350\n\tif (fsi->mount_opts.max_pages)\n\t\tseq_printf(m, ",size=%luk", fsi->mount_opts.max_pages << (PAGE_SHIFT - 10));\n#endif\n/; s/(\tOpt_mode,\n)(\};)/$1#ifdef CONFIG_ARCH_RP2350\n\tOpt_size,\n#endif\n$2/; s/(\tfsparam_u32oct\("mode",\tOpt_mode\),\n)/$1#ifdef CONFIG_ARCH_RP2350\n\tfsparam_string("size",\tOpt_size),\n#endif\n/; s/(\tcase Opt_mode:\n\t\tfsi->mount_opts\.mode = result\.uint_32 & S_IALLUGO;\n\t\tbreak;\n)/$1#ifdef CONFIG_ARCH_RP2350\n\tcase Opt_size:\n\t\treturn ramfs_rp_parse_size(param->string, &fsi->mount_opts.max_pages);\n#endif\n/' fs/ramfs/inode.c$N
perl -0pi -e 's/(\tret = inode_newsize_ok\(inode, newsize\);\n\tif \(ret\)\n\t\treturn ret;\n)/$1#ifdef CONFIG_ARCH_RP2350\n\tret = ramfs_rp_room(inode, (newsize + PAGE_SIZE - 1) >> PAGE_SHIFT);\n\tif (ret)\n\t\treturn ret;\n#endif\n/' fs/ramfs/file-nommu.c$N
printf '\n#ifdef CONFIG_ARCH_RP2350\nint ramfs_rp_room(struct inode *inode, unsigned long npages);\n#endif\n' >> fs/ramfs/internal.h$N
for f in fs/ramfs/inode.c:'#include "rp2350-size.inc"' fs/ramfs/inode.c:'a_ops = ramfs_rp_get_aops();' fs/ramfs/inode.c:'.statfs		= RAMFS_STATFS,' fs/ramfs/inode.c:'",size=%luk"' fs/ramfs/inode.c:'	Opt_size,' fs/ramfs/inode.c:'fsparam_string("size",	Opt_size),' fs/ramfs/inode.c:'case Opt_size:' fs/ramfs/file-nommu.c:'ret = ramfs_rp_room(inode,'; do
  grep -qF "${f#*:}" "${f%%:*}$N" || { echo "apply.sh: patching ${f%%:*} (${f#*:}) failed" >&2; exit 1; }
done
# n_tty keeps a 4 KiB read buffer and a 4 KiB echo buffer per tty, about
# 8.8 KiB, which nommu's vzalloc makes a 16 KiB (order 2) kmalloc: two per
# pty pair, 272 KiB with a few sessions and tmux up. 1 KiB each (canonical
# lines up to 1023 characters) makes it one 4 KiB slab object. And a tty
# buffers up to 640 KiB that nobody reads yet (a program writing to a pty
# whose reader, tmux, is swapped out): 16 KiB, then the writer waits.
perl -0pi -e 's/#define N_TTY_BUF_SIZE 4096\n/#ifdef CONFIG_ARCH_RP2350\n#define N_TTY_BUF_SIZE 1024\n#else\n#define N_TTY_BUF_SIZE 4096\n#endif\n/' include/linux/tty.h$N
perl -0pi -e 's/#define TTYB_DEFAULT_MEM_LIMIT\t\(640 \* 1024UL\)\n/#ifdef CONFIG_ARCH_RP2350\n#define TTYB_DEFAULT_MEM_LIMIT\t(16 * 1024UL)\n#else\n#define TTYB_DEFAULT_MEM_LIMIT\t(640 * 1024UL)\n#endif\n/' drivers/tty/tty_buffer.c$N
# fork's kernel stack (8 KiB, order 1) may fail: fork returns ENOMEM to the
# caller instead of the OOM killer taking a running process
perl -0pi -e 's/#define THREADINFO_GFP\t\t\(GFP_KERNEL_ACCOUNT \| __GFP_ZERO\)\n/#ifdef CONFIG_ARCH_RP2350\n#define THREADINFO_GFP\t\t(GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_RETRY_MAYFAIL)\n#else\n$&#endif\n/' include/linux/thread_info.h$N
# vm.panic_on_oom, vm.oom_kill_allocating_task and vm.oom_dump_tasks are
# registered by oom_init, which stock 6.12 builds only with an MMU (with the
# oom_reaper): on nommu the sysctls wouldn't exist, S02memlimits' echo would
# fail, and the OOM killer would take the biggest process, not the one
# asking. Close the MMU block before the table, and start the reaper only
# with an MMU.
perl -0pi -e 's/\n(#ifdef CONFIG_SYSCTL\nstatic struct ctl_table vm_oom_kill_table\[\] = \{)/\n#else\nstatic inline void queue_oom_reaper(struct task_struct *tsk)\n{\n}\n#endif \/* CONFIG_MMU *\/\n\n$1/; s/(\nstatic int __init oom_init\(void\)\n\{\n)(\toom_reaper_th = kthread_run\(oom_reaper, NULL, "oom_reaper"\);\n)/$1#ifdef CONFIG_MMU\n$2#endif\n/; s/(subsys_initcall\(oom_init\)\n)#else\nstatic inline void queue_oom_reaper\(struct task_struct \*tsk\)\n\{\n\}\n#endif \/\* CONFIG_MMU \*\/\n/$1/' mm/oom_kill.c$N
perl -0ne 'exit(!(/#ifdef CONFIG_MMU\n\toom_reaper_th/ && /\n\}\n#endif \/\* CONFIG_MMU \*\/\n\n#ifdef CONFIG_SYSCTL\nstatic struct ctl_table vm_oom_kill_table/ && /subsys_initcall\(oom_init\)\n\n/))' mm/oom_kill.c$N || { echo "apply.sh: patching mm/oom_kill.c failed" >&2; exit 1; }
# Stock oom_kill_allocating_task exempts only -1000, so it would take the
# daemon that lets you back in (dropbear, telnetd at -999, woken by a
# login) when its own small allocation is the one that fails. Here -999
# and -1000 are both never the allocating victim (the OOM killer picks
# among the rest, where they come last), and -999 is the daemons' own:
# exec resets it to 0, so the sessions and commands they start don't
# inherit it (-1000 is inherited, as in stock Linux).
perl -0pi -e 's/(\t    oom_cpuset_eligible\(current, oc\) &&\n)\t    current->signal->oom_score_adj != OOM_SCORE_ADJ_MIN\) \{/$1#ifdef CONFIG_ARCH_RP2350\n\t    current->signal->oom_score_adj > OOM_SCORE_ADJ_MIN + 1) {\n#else\n\t    current->signal->oom_score_adj != OOM_SCORE_ADJ_MIN) {\n#endif/' mm/oom_kill.c$N
perl -0pi -e 's/(\t\t__set_task_comm\(me, kbasename\(bprm->filename\), true\);\n\t\}\n)/$1#ifdef CONFIG_ARCH_RP2350\n\tif (me->signal->oom_score_adj == OOM_SCORE_ADJ_MIN + 1)\n\t\tme->signal->oom_score_adj = 0;\t\/* -999: the daemon only, not what it runs *\/\n#endif\n/' fs/exec.c$N
for f in mm/oom_kill.c:'oom_score_adj > OOM_SCORE_ADJ_MIN + 1) {' fs/exec.c:'-999: the daemon only'; do
  grep -qF -e "${f#*:}" "${f%%:*}$N" || { echo "apply.sh: patching ${f%%:*} (oom) failed" >&2; exit 1; }
done
for f in include/linux/tty.h:'#define N_TTY_BUF_SIZE 1024' drivers/tty/tty_buffer.c:'(16 * 1024UL)' include/linux/thread_info.h:'__GFP_ZERO | __GFP_RETRY_MAYFAIL)'; do
  grep -qF "${f#*:}" "${f%%:*}$N" || { echo "apply.sh: patching ${f%%:*} failed" >&2; exit 1; }
done

# ---- memory reporting ----
# What free, top, htop, ps and the OOM killer read, told the truth about
# the ATRANS pool and swap. The pool is taken from the page allocator, so
# its free space counts as used: add it to MemFree (and sysinfo's freeram)
# and MemAvailable. ATRANS swap isn't CONFIG_SWAP: si_swapinfo reports it
# (SwapTotal/SwapFree, sysinfo's totalswap/freeswap), while
# total_swap_pages stays 0, so reclaim never tries to swap LRU pages.
perl -0pi -e "$(cat <<'PERL'
s~(#include "internal.h"\n)~#ifdef CONFIG_RP2350_ATRANS\n#include <asm/rp2350-atrans.h>\n#endif\n$1~;
s~(\tavailable = global_zone_page_state\(NR_FREE_PAGES\) - totalreserve_pages;\n)~$1#ifdef CONFIG_RP2350_ATRANS\n\tavailable += rp2350_atrans_free_pages();\t/* rp2350: the ATRANS pool */\n#endif\n~;
s~(\tval->freeram = global_zone_page_state\(NR_FREE_PAGES\);\n)~$1#ifdef CONFIG_RP2350_ATRANS\n\tval->freeram += rp2350_atrans_free_pages();\t/* rp2350: the ATRANS pool */\n#endif\n~;
PERL
)" mm/show_mem.c$N
perl -0pi -e "$(cat <<'PERL'
s~#define si_swapinfo\(val\) \\\n\tdo \{ \(val\)->freeswap = \(val\)->totalswap = 0; \} while \(0\)\n~#ifdef CONFIG_RP2350_ATRANS_SWAP\n#include <asm/rp2350-atrans.h>\n#define si_swapinfo(val) rp2350_atrans_si_swapinfo(val)\t/* rp2350: ATRANS swap */\n#else\n$&#endif\n~;
PERL
)" include/linux/swap.h$N
# Per-process memory: nommu keeps no RSS counters, so RSS read 0 everywhere
# (/proc/<pid>/stat, status, statm, getrusage) and the OOM killer couldn't
# weigh a process. The ATRANS driver keeps an ATRANS process's (block,
# shared text, swap: set_block in rp2350-atrans.c); nommu.c keeps an FDPIC
# process's, per mapping, as it maps and unmaps; status and statm print
# them as an MMU kernel does (fs/proc/rp2350-task.inc).
perl -0pi -e "$(cat <<'PERL'
s~(static void setup_vma_to_mm\(struct vm_area_struct \*vma, struct mm_struct \*mm\)\n)~#ifdef CONFIG_ARCH_RP2350\n/*\n * rp2350: RSS counters, which stock nommu does not keep. A mapping counts\n * while it is mapped, if its pages are RAM (text run in place from flash or\n * SRAM costs none): a private writable one as anonymous memory (the copy\n * belongs to the process), a read-only or shared file mapping as file\n * memory.\n */\nstatic void rp2350_vma_rss(struct vm_area_struct *vma, long pages)\n{\n\tif (!pages || !vma->vm_mm || !virt_addr_valid((void *)vma->vm_start))\n\t\treturn;\n\tif (pages < 0)\n\t\tupdate_hiwater_rss(vma->vm_mm);\n\tpercpu_counter_add(&vma->vm_mm->rss_stat[vma->vm_file && (!(vma->vm_flags & VM_WRITE) ||\n\t\t\t   (vma->vm_flags & VM_SHARED)) ? MM_FILEPAGES : MM_ANONPAGES], pages);\n}\n#else\nstatic inline void rp2350_vma_rss(struct vm_area_struct *vma, long pages)\n{\n}\n#endif\n\n$1~;
s~(static void cleanup_vma_from_mm\(struct vm_area_struct \*vma\)\n\{\n)~$1\trp2350_vma_rss(vma, -(long)vma_pages(vma));\n~;
s~(\tsetup_vma_to_mm\(vma, current->mm\);\n\tcurrent->mm->map_count\+\+;\n)~$1\trp2350_vma_rss(vma, vma_pages(vma));\n~;
s~(\t\tvma->vm_start = to;\n\t\}\n)(\n\t/\* cut the backing region down to size \*/\n)~$1\trp2350_vma_rss(vma, -(long)((to - from) >> PAGE_SHIFT));\n$2~;
s~(\t/\* all checks complete - do it \*/\n)(\tvma->vm_end = vma->vm_start \+ new_len;\n)~$1\trp2350_vma_rss(vma, (long)(new_len >> PAGE_SHIFT) - (long)(old_len >> PAGE_SHIFT));\n$2~;
PERL
)" mm/nommu.c$N
cmp -s "$H/fs/proc/rp2350-task.inc" fs/proc/rp2350-task.inc || cp "$H/fs/proc/rp2350-task.inc" fs/proc/rp2350-task.inc
perl -0pi -e "$(cat <<'PERL'
s~(#include "internal.h"\n)~$1#ifdef CONFIG_ARCH_RP2350\n#include "rp2350-task.inc"\n#endif\n~;
s~(\tunsigned long bytes = 0, sbytes = 0, slack = 0, size;\n)~$1\n#ifdef CONFIG_ARCH_RP2350\n\trp2350_task_mem(m, mm);\n\treturn;\n#endif\n~;
s~(\tunsigned long size = kobjsize\(mm\);\n)~$1\n#ifdef CONFIG_ARCH_RP2350\n\treturn rp2350_task_statm(mm, shared, text, data, resident);\n#endif\n~;
PERL
)" fs/proc/task_nommu.c$N
# The OOM killer's badness: RSS (now real), not swap. ATRANS swap holds
# whole processes, and killing one that is swapped out frees no RAM.
perl -0pi -e "$(cat <<'PERL'
s~(\tpoints = get_mm_rss\(p->mm\) )\+ get_mm_counter\(p->mm, MM_SWAPENTS\) \+\n~#ifdef CONFIG_RP2350_ATRANS_SWAP\n\t/* rp2350: not swap: killing a swapped-out ATRANS process frees no RAM */\n$1+\n#else\n$&#endif\n~;
PERL
)" mm/oom_kill.c$N
for f in mm/show_mem.c:'available += rp2350_atrans_free_pages();' mm/show_mem.c:'val->freeram += rp2350_atrans_free_pages();' mm/show_mem.c:'#include <asm/rp2350-atrans.h>' include/linux/swap.h:'#define si_swapinfo(val) rp2350_atrans_si_swapinfo(val)' mm/nommu.c:'static void rp2350_vma_rss(' mm/nommu.c:'rp2350_vma_rss(vma, -(long)vma_pages(vma));' mm/nommu.c:'rp2350_vma_rss(vma, vma_pages(vma));' mm/nommu.c:'rp2350_vma_rss(vma, -(long)((to - from)' mm/nommu.c:'rp2350_vma_rss(vma, (long)(new_len' fs/proc/task_nommu.c:'#include "rp2350-task.inc"' fs/proc/task_nommu.c:'rp2350_task_mem(m, mm);' fs/proc/task_nommu.c:'return rp2350_task_statm(' mm/oom_kill.c:'frees no RAM */'; do
  grep -qF -e "${f#*:}" "${f%%:*}$N" || { echo "apply.sh: patching ${f%%:*} (${f#*:}) failed" >&2; exit 1; }
done

# ---- user-mode faults deliver signals (mach-rp2350/fault.c) ----
# Stock v7-M sends MemManage/BusFault/UsageFault to __invalid_entry, which
# hangs; route them to __fault_entry (v7m_fault, then PendSV for the signal).
perl -0pi -e 's/(ENDPROC\(__pendsv_entry\)\n)/$1\n#ifdef CONFIG_ARCH_RP2350\n\/*\n * MemManage, BusFault, UsageFault: v7m_fault() (mach-rp2350\/fault.c) sends\n * a user fault its signal and returns; PendSV then delivers the signal on\n * the way back to user mode. It does not return for a kernel fault.\n *\/\n\t.align\t2\n__fault_entry:\n\t\@ a fault while stacking onto the process stack (MSTKERR, STKERR)\n\t\@ left no frame there: hand v7m_exception_entry a harmless one\n\ttst\tlr, #EXC_RET_STACK_MASK\n\tbeq\t1f\n\tldr\tr0, =BASEADDR_V7M_SCB\n\tldr\tr0, [r0, #0x28]\t\t\@ CFSR\n\ttst\tr0, #0x10\t\t\@ MSTKERR\n\tit\teq\n\ttsteq\tr0, #0x1000\t\t\@ STKERR\n\tbeq\t1f\n\tmrs\tr0, psp\n\tldr\tr1, =v7m_stkerr_psp\n\tstr\tr0, [r1]\n\tldr\tr0, =v7m_stkerr_frame\n\tmsr\tpsp, r0\n1:\tv7m_exception_entry\n\tmov\tr0, sp\n\tmov\tr1, lr\n\tmov\tr4, lr\t\t\t\@ r4 is reloaded from pt_regs below\n\tbl\tv7m_fault\n\tmov\tlr, r4\n\tldr\tr1, =BASEADDR_V7M_SCB\n\tmov\tr0, #V7M_SCB_ICSR_PENDSVSET\n\tstr\tr0, [r1, V7M_SCB_ICSR]\t\@ raise PendSV\n\tldmia\tsp!, {r0-r11}\n\tadd\tsp, #PT_REGS_SIZE-S_IP\n\tcpsie\ti\n\tbx\tlr\nENDPROC(__fault_entry)\n#define V7M_FAULT_ENTRY __fault_entry\n#else\n#define V7M_FAULT_ENTRY __invalid_entry\n#endif\n/; s/__invalid_entry(\t\t\@ [456] - (MemManage|BusFault|UsageFault))/V7M_FAULT_ENTRY$1/g' arch/arm/kernel/entry-v7m.S$N
# a fault the kernel can't handle (HardFault, or one escalated to it with
# interrupts off): the fault status and ATRANS state, then show_regs
perl -0pi -e 's/(\tbl\t_printk\n#endif\n)(\tmov\tr0, sp\n\tbl\tshow_regs\n)/$1#ifdef CONFIG_RP2350_ATRANS\n\tbl\trp2350_atrans_fault_dump\n#endif\n$2/' arch/arm/kernel/entry-v7m.S$N
grep -qF 'bl	rp2350_atrans_fault_dump' arch/arm/kernel/entry-v7m.S$N || { echo "apply.sh: patching entry-v7m.S (fault dump) failed" >&2; exit 1; }
# uaccess (nommu): ATRANS processes may only pass pointers into their window.
# Stock nommu access_ok() accepts everything and get_user/put_user don't check.
# get_user evaluates p once: callers pass p++ (random.c's RNDADDENTROPY reads
# get_user(ent_count, p++); get_user(len, p++)); evaluated twice, it would
# skip a word each time and take len from the seed bytes, and the copy would
# run off the end of RAM into the text window (a BusFault in seedrng at
# shutdown).
perl -0pi -e 's/#define get_user\(x, p\)\t__get_user\(x, p\)\n#define __put_user_check __put_user_nocheck\n/#ifdef CONFIG_RP2350_ATRANS\nbool rp2350_atrans_access_ok(unsigned long addr, unsigned long size);\n#define __access_ok(p, s) rp2350_atrans_access_ok((unsigned long)(p), (s))\nunsigned long rp2350_atrans_task_size_max(void);\n#define TASK_SIZE_MAX rp2350_atrans_task_size_max()\n#define get_user(x, p)\t\t\t\t\t\t\t\\\n\t({\t\t\t\t\t\t\t\t\\\n\t\t__typeof__(p) __gu_p = (p);\t\t\t\t\\\n\t\taccess_ok(__gu_p, sizeof(*__gu_p)) ? __get_user(x, __gu_p) :\t\\\n\t\t\t((x) = (__typeof__(*__gu_p))0, -EFAULT);\t\t\\\n\t})\n#define __put_user_check(x, __pu_ptr, __err, __size)\t\t\t\\\n\tdo {\t\t\t\t\t\t\t\t\\\n\t\tif (access_ok(__pu_ptr, __size))\t\t\t\\\n\t\t\t__put_user_nocheck(x, __pu_ptr, __err, __size);\t\\\n\t\telse\t\t\t\t\t\t\t\\\n\t\t\t(__err) = -EFAULT;\t\t\t\t\\\n\t} while (0)\n#else\n$&#endif\n/' arch/arm/include/asm/uaccess.h$N
# ATRANS v3 shared text: user pointers into the text window may be read
# (access_ok accepts them), but the MPU makes the text read-only for the
# kernel too, so every write path refuses it: put_user/__put_user,
# copy_to_user, clear_user, the futex ops. strnlen_user/strncpy_from_user
# stop at the end of the region holding the string.
perl -0pi -e 's/(#define TASK_SIZE_MAX rp2350_atrans_task_size_max\(\)\n)/$1#define rp2350_atrans_ro(p) ((unsigned long)(p) - 0x11c00000UL < 0x400000UL)\nunsigned long rp2350_atrans_str_max(unsigned long addr);\n/; s/(#define __put_user_check __put_user_nocheck\n)#endif\n/$1#define rp2350_atrans_ro(p) 0\n#endif\n/; s/(\t\tunsigned long __pu_addr = \(unsigned long\)__pu_ptr;\t\\\n)(\t\t__put_user_nocheck_##__size\(x, __pu_addr, __err, TUSER\(\)\);\\\n)/$1\t\tif (rp2350_atrans_ro(__pu_addr))\t\t\t\\\n\t\t\t(__err) = -EFAULT;\t\t\t\t\\\n\t\telse\t\t\t\t\t\t\t\\\n\t$2/; s/(raw_copy_to_user\(void __user \*to, const void \*from, unsigned long n\)\n\{\n)(\tmemcpy\(\(void __force \*\)to, from, n\);\n)/$1\tif (rp2350_atrans_ro(to))\n\t\treturn n;\n$2/; s/#define __clear_user\(addr, n\)\t\t\(memset\(\(void __force \*\)addr, 0, n\), 0\)/#define __clear_user(addr, n)\t\t(rp2350_atrans_ro(addr) ? (n) : (memset((void __force *)addr, 0, n), 0))/' arch/arm/include/asm/uaccess.h$N
perl -0pi -e 's/\tif \(!access_ok\(uaddr, sizeof\(u32\)\)\)\n\t\treturn -EFAULT;/\tif (!access_ok(uaddr, sizeof(u32)) || rp2350_atrans_ro(uaddr))\n\t\treturn -EFAULT;/g' arch/arm/include/asm/futex.h$N
perl -0pi -e 's/\tmax_addr = TASK_SIZE_MAX;\n/#ifdef CONFIG_RP2350_ATRANS\n\tmax_addr = rp2350_atrans_str_max((unsigned long)str);\n#else\n$&#endif\n/' lib/strnlen_user.c$N
perl -0pi -e 's/\tmax_addr = TASK_SIZE_MAX;\n/#ifdef CONFIG_RP2350_ATRANS\n\tmax_addr = rp2350_atrans_str_max((unsigned long)src);\n#else\n$&#endif\n/' lib/strncpy_from_user.c$N
# CONFIG_RP2350_SRAM_DATA: the XIP kernel's RAM part linked into SRAM, which
# is outside the page allocator's memory, so its init data must not be freed
perl -0pi -e 's/(\t__data_loc = ALIGN\(4\);\t\t\/\* location in file \*\/\n)\t\. = PAGE_OFFSET \+ TEXT_OFFSET;\t\/\* location in memory \*\/\n#undef LOAD_OFFSET\n#define LOAD_OFFSET \(PAGE_OFFSET \+ TEXT_OFFSET - __data_loc\)\n/#ifdef CONFIG_RP2350_SRAM_DATA\n#define XIP_DATA_ADDR 0x20000000\n#else\n#define XIP_DATA_ADDR (PAGE_OFFSET + TEXT_OFFSET)\n#endif\n$1\t. = XIP_DATA_ADDR;\t\t\/* location in memory *\/\n#undef LOAD_OFFSET\n#define LOAD_OFFSET (XIP_DATA_ADDR - __data_loc)\n/' arch/arm/kernel/vmlinux-xip.lds.S$N
perl -0pi -e 's/(\tif \(!machine_is_integrator\(\) && !machine_is_cintegrator\(\)\)\n)(\t\tfree_initmem_default\(-1\);\n)/#ifndef CONFIG_RP2350_SRAM_DATA\t\/* init data is in SRAM, which has no struct pages *\/\n$1$2#endif\n/' arch/arm/mm/init.c$N
# physmap rounds a window down to a power of two (for GPIO-banked flash);
# the sram-text RAM MTD (480 KiB) needs its exact size
perl -0pi -e 's/(\t\tinfo->win_order = fls64\(resource_size\(res\)\) - 1;\n)(\t\tinfo->maps\[i\]\.size = BIT\(info->win_order \+\n\t\t\t\t\t \(info->gpios \?\n\t\t\t\t\t  info->gpios->ndescs : 0\)\);\n)/$1$2\t\tif (!info->gpios)\t\/* rp2350: exact size without address GPIOs *\/\n\t\t\tinfo->maps[i].size = resource_size(res);\n/' drivers/mtd/maps/physmap-core.c$N
# TLS: set_tls also updates the user-readable thread pointer word
perl -0pi -e 's/(asmlinkage int arm_syscall\(int no, struct pt_regs \*regs\)\n)/#ifdef CONFIG_RP2350_ATRANS\n#include <asm\/rp2350-atrans.h>\n#endif\n\n$1/; s/(\tcase NR\(set_tls\):\n\t\tset_tls\(regs->ARM_r0\);\n)/$1#ifdef CONFIG_RP2350_ATRANS\n\t\trp2350_atrans_set_tls(regs->ARM_r0);\n#endif\n/' arch/arm/kernel/traps.c$N
# the kernel writes the exception frame below the user sp with privileged
# stores on every return to user mode: for an ATRANS process the sp from
# sigreturn and clone's new stack must be inside its block
perl -0pi -e 's/(static inline int valid_user_regs\(struct pt_regs \*regs\)\n)/#ifdef CONFIG_RP2350_ATRANS\nint rp2350_atrans_user_regs_ok(struct pt_regs *regs);\n#endif\n\n$1/; s/(#else \/\* ifndef CONFIG_CPU_V7M \*\/\n)\treturn 1;\n/$1#ifdef CONFIG_RP2350_ATRANS\n\treturn rp2350_atrans_user_regs_ok(regs);\n#endif\n\treturn 1;\n/' arch/arm/include/asm/ptrace.h$N
perl -0pi -e 's/(#include "signal.h"\n)/$1#ifdef CONFIG_RP2350_ATRANS\n#include <asm\/rp2350-atrans.h>\n#endif\n/; s/\t\tif \(stack_start\)\n\t\t\tchildregs->ARM_sp = stack_start;\n/\t\tif (stack_start) {\n#ifdef CONFIG_RP2350_ATRANS\n\t\t\tif (!rp2350_atrans_sp_ok(p->mm, stack_start))\n\t\t\t\treturn -EINVAL;\n#endif\n\t\t\tchildregs->ARM_sp = stack_start;\n\t\t}\n/' arch/arm/kernel/process.c$N
# futex keys: nommu keys a private futex by address alone (one address
# space); ATRANS processes share window addresses, so key them by mm too
perl -0pi -e 's/\t\tif \(IS_ENABLED\(CONFIG_MMU\)\)\n(\t\t\tkey->private.mm = mm;)/#ifdef CONFIG_RP2350_ATRANS\n\t\tif (IS_ENABLED(CONFIG_MMU) || (mm \&\& mm->context.atrans_kva))\t\/* rp2350 *\/\n#else\n\t\tif (IS_ENABLED(CONFIG_MMU))\n#endif\n$1/' kernel/futex/core.c$N
# /proc/<pid>/maps and VSZ for ATRANS processes (they have no VMAs)
perl -0pi -e 's/(#include "internal.h"\n)/$1#ifdef CONFIG_RP2350_ATRANS\n#include <asm\/rp2350-atrans.h>\n#define ATRANS_MAPS ((void *)1UL)\t\/* m_start token: show the block *\/\n#endif\n/; s/(unsigned long task_vsize\(struct mm_struct \*mm\)\n\{\n\tVMA_ITERATOR\(vmi, mm, 0\);\n\tstruct vm_area_struct \*vma;\n\tunsigned long vsize = 0;\n\n)/$1#ifdef CONFIG_RP2350_ATRANS\n\tif (mm->context.atrans)\n\t\treturn rp2350_atrans_vsize(mm);\n#endif\n/; s/(static int show_map\(struct seq_file \*m, void \*_p\)\n\{\n)/$1#ifdef CONFIG_RP2350_ATRANS\n\tif (_p == ATRANS_MAPS)\n\t\treturn rp2350_atrans_show_maps(m, ((struct proc_maps_private *)m->private)->mm);\n#endif\n/; s/(\tvma_iter_init\(&priv->iter, mm, last_addr\);\n)/#ifdef CONFIG_RP2350_ATRANS\n\tif (mm->context.atrans)\n\t\treturn ATRANS_MAPS;\n#endif\n$1/; s/(static void \*m_next\(struct seq_file \*m, void \*_p, loff_t \*ppos\)\n\{\n)/$1#ifdef CONFIG_RP2350_ATRANS\n\tif (_p == ATRANS_MAPS) {\n\t\t*ppos = -1UL;\n\t\treturn NULL;\n\t}\n#endif\n/' fs/proc/task_nommu.c$N
for f in drivers/mtd/maps/physmap-core.c:'rp2350: exact size' arch/arm/kernel/vmlinux-xip.lds.S:'. = XIP_DATA_ADDR;' arch/arm/mm/init.c:'#ifndef CONFIG_RP2350_SRAM_DATA' arch/arm/Kconfig:'config ARCH_RP2350' arch/arm/Makefile:'CONFIG_ARCH_RP2350' arch/arm/include/asm/mmu.h:atrans_kva arch/arm/include/asm/mmu_context.h:'rp2350_atrans_switch(next)' kernel/fork.c:'return rp2350_atrans_dup' kernel/fork.c:'return kernel_clone(&args);' mm/nommu.c:'rp2350_atrans_exit(mm);' mm/nommu.c:'return rp2350_atrans_mmap(file' mm/nommu.c:'return rp2350_atrans_munmap(mm' mm/nommu.c:'return rp2350_atrans_brk(mm, brk);' mm/nommu.c:'return rp2350_atrans_access(mm' arch/arm/include/asm/mmu.h:'*atrans;' arch/arm/kernel/entry-v7m.S:'v7m_stkerr_psp' arch/arm/kernel/traps.c:'rp2350_atrans_set_tls(regs->ARM_r0);' arch/arm/include/asm/ptrace.h:'return rp2350_atrans_user_regs_ok(regs);' arch/arm/kernel/process.c:'rp2350_atrans_sp_ok(p->mm, stack_start)' kernel/futex/core.c:'atrans_kva))	/* rp2350' fs/proc/task_nommu.c:'return rp2350_atrans_vsize(mm);' fs/proc/task_nommu.c:'return rp2350_atrans_show_maps(m' fs/proc/task_nommu.c:'return ATRANS_MAPS;' fs/proc/task_nommu.c:'*ppos = -1UL;' arch/arm/include/asm/uaccess.h:'if (rp2350_atrans_ro(__pu_addr))' arch/arm/include/asm/uaccess.h:'if (rp2350_atrans_ro(to))' arch/arm/include/asm/uaccess.h:'(rp2350_atrans_ro(addr) ?' arch/arm/include/asm/uaccess.h:'define rp2350_atrans_ro(p) 0' arch/arm/include/asm/futex.h:'|| rp2350_atrans_ro(uaddr))' lib/strnlen_user.c:'rp2350_atrans_str_max((unsigned long)str)' lib/strncpy_from_user.c:'rp2350_atrans_str_max((unsigned long)src)' arch/arm/kernel/entry-v7m.S:'__fault_entry:' arch/arm/include/asm/uaccess.h:'define __access_ok(p, s) rp2350'; do
  grep -qF "${f#*:}" "${f%%:*}$N" || { echo "apply.sh: patching ${f%%:*} failed" >&2; exit 1; }
done
for f in $FILES; do
  if cmp -s "$f$N" "$f"; then rm "$f$N"; else mv "$f$N" "$f"; fi
done
