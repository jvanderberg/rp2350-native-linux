// SPDX-License-Identifier: GPL-2.0
/*
 * /proc/<pid>/status and /proc/<pid>/statm on nommu, from the mm's RSS
 * counters, in the MMU kernel's format. Stock nommu keeps no counters: its
 * status shows a Mem/Slack/Shared summary and its statm counts every mapping
 * as resident. Here the counters are kept (mm/nommu.c for FDPIC mappings,
 * drivers/rp2350/rp2350-atrans.c for an ATRANS process's block, its shared
 * text and what swap holds of it), so VmRSS, VmSwap and statm are real.
 * Included by fs/proc/task_nommu.c (linux/apply.sh).
 */
#ifdef CONFIG_RP2350_ATRANS
#include <asm/rp2350-atrans.h>
#endif

/* text and data, in pages */
static void rp2350_text_data(struct mm_struct *mm, unsigned long *text, unsigned long *data)
{
#ifdef CONFIG_RP2350_ATRANS
	if (mm->context.atrans) {
		rp2350_atrans_statm(mm, text, data);	/* the text block, the data block */
		return;
	}
#endif
	/* FDPIC: the program's own text, and its private memory */
	*text = mm->end_code > mm->start_code ?
		(PAGE_ALIGN(mm->end_code) - (mm->start_code & PAGE_MASK)) >> PAGE_SHIFT : 0;
	*data = get_mm_counter_sum(mm, MM_ANONPAGES);
}

static unsigned long rp2350_task_statm(struct mm_struct *mm, unsigned long *shared,
				       unsigned long *text, unsigned long *data,
				       unsigned long *resident)
{
	*shared = get_mm_counter_sum(mm, MM_FILEPAGES) + get_mm_counter_sum(mm, MM_SHMEMPAGES);
	*resident = *shared + get_mm_counter_sum(mm, MM_ANONPAGES);
	rp2350_text_data(mm, text, data);
	return task_vsize(mm) >> PAGE_SHIFT;
}

#define RP_PUT_KB(str, pages) seq_put_decimal_ull_width(m, str, (pages) << (PAGE_SHIFT - 10), 8)
static void rp2350_task_mem(struct seq_file *m, struct mm_struct *mm)
{
	unsigned long anon, file, shmem, rss, text, data;

	anon = get_mm_counter_sum(mm, MM_ANONPAGES);
	file = get_mm_counter_sum(mm, MM_FILEPAGES);
	shmem = get_mm_counter_sum(mm, MM_SHMEMPAGES);
	rss = anon + file + shmem;
	rp2350_text_data(mm, &text, &data);
	seq_put_decimal_ull_width(m, "VmSize:\t", task_vsize(mm) >> 10, 8);
	RP_PUT_KB(" kB\nVmHWM:\t", max(rss, mm->hiwater_rss));
	RP_PUT_KB(" kB\nVmRSS:\t", rss);
	RP_PUT_KB(" kB\nRssAnon:\t", anon);
	RP_PUT_KB(" kB\nRssFile:\t", file);
	RP_PUT_KB(" kB\nRssShmem:\t", shmem);
	RP_PUT_KB(" kB\nVmData:\t", data);
	RP_PUT_KB(" kB\nVmExe:\t", text);
	RP_PUT_KB(" kB\nVmSwap:\t", get_mm_counter_sum(mm, MM_SWAPENTS));
	seq_puts(m, " kB\n");
}
#undef RP_PUT_KB
