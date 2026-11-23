/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hooks from the core kernel into drivers/rp2350/rp2350-atrans.c
 * (per-process ATRANS address spaces). Installed by linux/apply.sh.
 */
#ifndef __ASM_RP2350_ATRANS_H
#define __ASM_RP2350_ATRANS_H

#ifdef CONFIG_RP2350_ATRANS
#include <linux/types.h>

struct file;
struct mm_struct;
struct pt_regs;
struct seq_file;

void rp2350_atrans_switch(struct mm_struct *next);
void rp2350_atrans_fault_dump(void);	/* __invalid_entry (entry-v7m.S) */
int rp2350_atrans_dup(struct mm_struct *oldmm, struct mm_struct *mm);
void rp2350_atrans_exit(struct mm_struct *mm);
unsigned long rp2350_atrans_brk(struct mm_struct *mm, unsigned long brk);
unsigned long rp2350_atrans_mmap(struct file *file, unsigned long addr, unsigned long len,
				 unsigned long prot, unsigned long flags, unsigned long pgoff);
int rp2350_atrans_munmap(struct mm_struct *mm, unsigned long start, size_t len);
int rp2350_atrans_access(struct mm_struct *mm, unsigned long addr, void *buf, int len, int write);
int rp2350_atrans_show_maps(struct seq_file *m, struct mm_struct *mm);
unsigned long rp2350_atrans_vsize(struct mm_struct *mm);
void rp2350_atrans_set_tls(unsigned long tp);
int rp2350_atrans_sp_ok(struct mm_struct *mm, unsigned long sp);
int rp2350_atrans_user_regs_ok(struct pt_regs *regs);
bool rp2350_atrans_access_ok(unsigned long addr, unsigned long size);
unsigned long rp2350_atrans_task_size_max(void);
unsigned long rp2350_atrans_stkerr_frame(void);
unsigned long rp2350_atrans_str_max(unsigned long addr);
void *rp2350_alloc_exact(size_t size);		/* mm/nommu.c: do_mmap_private */
/* memory reporting: mm/show_mem.c, fs/proc/rp2350-task.inc, /proc/meminfo */
unsigned long rp2350_atrans_free_pages(void);	/* free space in the ATRANS pool */
void rp2350_atrans_statm(struct mm_struct *mm, unsigned long *text, unsigned long *data);
void arch_report_meminfo(struct seq_file *m);
#endif

#ifdef CONFIG_RP2350_ATRANS_SWAP
struct sysinfo;

void rp2350_atrans_swap_resume(void);	/* end of schedule(): swap current back in */
void rp2350_atrans_si_swapinfo(struct sysinfo *val);	/* si_swapinfo (linux/swap.h) */
#endif

#endif
