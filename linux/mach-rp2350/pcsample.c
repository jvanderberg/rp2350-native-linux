// SPDX-License-Identifier: GPL-2.0
/*
 * /proc/pcsample: a sampling profiler for measurements. TIMER0's ALARM1
 * (TIMER0_IRQ_1) is routed to the NMI (EPPB NMI_MASK0), so samples land
 * inside interrupts-off code too (the XIP cache flush, the scheduler).
 * Each sample is two words: the interrupted PC (bit 0 set: user mode, from
 * PSP) and current's pid.
 *
 *   echo PERIOD_US > /proc/pcsample   start (clears the buffer)
 *   echo 0 > /proc/pcsample           stop
 *   cat /proc/pcsample > file         the samples, raw little-endian words
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#define TIMER0		0x400b0000
#define T_ALARM1	((void __iomem *)(TIMER0 + 0x14))
#define T_ARMED		((void __iomem *)(TIMER0 + 0x20))
#define T_RAWL		((void __iomem *)(TIMER0 + 0x28))
#define T_INTR		((void __iomem *)(TIMER0 + 0x3c))
#define T_INTE_SET	((void __iomem *)(TIMER0 + 0x2000 + 0x40))
#define T_INTE_CLR	((void __iomem *)(TIMER0 + 0x3000 + 0x40))
#define EPPB_NMI_MASK0	((void __iomem *)0xe0080000)

#define NSAMP	(24 * 1024)

extern unsigned long vector_table[];

static u32 *buf;
static volatile u32 n;
static u32 period, seed = 1;

void pcs_nmi_c(u32 *frame, u32 excret);

void pcs_nmi_c(u32 *frame, u32 excret)
{
	u32 pc = frame[6];

	writel_relaxed(2, T_INTR);
	if (n < NSAMP) {
		buf[2 * n] = (pc & ~1U) | (excret & 4 ? 1 : 0);
		buf[2 * n + 1] = current->pid;
		n++;
	}
	if (period && n < NSAMP) {
		seed = seed * 1103515245 + 12345;
		writel_relaxed(readl_relaxed(T_RAWL) + period - period / 8 +
			       ((seed >> 16) % (period / 4 + 1)), T_ALARM1);
	}
}

void __attribute__((naked)) pcs_nmi(void);
void __attribute__((naked)) pcs_nmi(void)
{
	asm volatile(
		"tst	lr, #4\n"
		"ite	eq\n"
		"mrseq	r0, msp\n"
		"mrsne	r0, psp\n"
		"mov	r1, lr\n"
		"push	{r4, lr}\n"
		"bl	pcs_nmi_c\n"
		"pop	{r4, pc}\n");
}

static ssize_t pcs_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
	return simple_read_from_buffer(ubuf, len, pos, buf, n * 8);
}

static ssize_t pcs_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	char s[16];
	unsigned long p;

	if (len >= sizeof(s) || copy_from_user(s, ubuf, len))
		return -EINVAL;
	s[len] = 0;
	if (kstrtoul(strim(s), 0, &p))
		return -EINVAL;
	period = 0;
	writel_relaxed(1 << 1, T_ARMED);
	writel_relaxed(2, T_INTR);
	if (p) {
		n = 0;
		period = p < 20 ? 20 : p;
		writel_relaxed(readl_relaxed(T_RAWL) + period, T_ALARM1);
	}
	return len;
}

static const struct proc_ops pcs_ops = {
	.proc_read = pcs_read,
	.proc_write = pcs_write,
	.proc_lseek = default_llseek,
};

static unsigned long boot_period;

static int __init pcs_setup(char *str)
{
	return !kstrtoul(str, 0, &boot_period);
}
__setup("pcsample=", pcs_setup);

static int __init pcs_init(void)
{
	buf = vmalloc(NSAMP * 8);
	if (!buf)
		return -ENOMEM;
	vector_table[2] = (unsigned long)pcs_nmi | 1;
	writel_relaxed(1 << 1, T_ARMED);
	writel_relaxed(2, T_INTR);
	writel_relaxed(2, T_INTE_SET);
	writel_relaxed(readl_relaxed(EPPB_NMI_MASK0) | 2, EPPB_NMI_MASK0);
	proc_create("pcsample", 0644, NULL, &pcs_ops);
	if (boot_period) {
		period = boot_period;
		writel_relaxed(readl_relaxed(T_RAWL) + period, T_ALARM1);
	}
	return 0;
}
late_initcall(pcs_init);
