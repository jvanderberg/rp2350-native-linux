// SPDX-License-Identifier: GPL-2.0
/*
 * /proc/xipcache: the XIP cache's hit and access counters (XIP_CTRL CTR_HIT
 * and CTR_ACC, 32-bit saturating). The cache serves both flash (kernel
 * text) and PSRAM (kernel RAM, user memory). Writing anything clears them.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#define XIP_CTR_HIT	((void __iomem *)0x400c800c)
#define XIP_CTR_ACC	((void __iomem *)0x400c8010)

static int xipcache_show(struct seq_file *m, void *v)
{
	u32 hit = readl_relaxed(XIP_CTR_HIT), acc = readl_relaxed(XIP_CTR_ACC);
	u32 permille = acc ? (u32)div_u64(1000ULL * hit, acc) : 0;

	seq_printf(m, "hits %u accesses %u misses %u hit%% %u.%u\n", hit, acc, acc - hit,
		   permille / 10, permille % 10);
	return 0;
}

static int xipcache_open(struct inode *inode, struct file *file)
{
	return single_open(file, xipcache_show, NULL);
}

static ssize_t xipcache_write(struct file *file, const char __user *buf, size_t n, loff_t *pos)
{
	writel_relaxed(0, XIP_CTR_HIT);
	writel_relaxed(0, XIP_CTR_ACC);
	return n;
}

static const struct proc_ops xipcache_ops = {
	.proc_open = xipcache_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = xipcache_write,
};

static int __init xipstat_init(void)
{
	proc_create("xipcache", 0644, NULL, &xipcache_ops);
	return 0;
}
device_initcall(xipstat_init);
