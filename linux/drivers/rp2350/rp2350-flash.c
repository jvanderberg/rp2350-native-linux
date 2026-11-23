// SPDX-License-Identifier: GPL-2.0
/*
 * RP2350 flash regions as NOR MTD devices: /home (jffs2, 1 MiB at
 * 0x10100000) and a scratch region for tests.
 *
 * The kernel runs in place from this flash, and its RAM is the PSRAM on the
 * same QMI, so nothing on core 0 may touch either while the flash erases or
 * programs. Erase and program go through the boot stub's routine in SRAM
 * (boot/src/flash.rs; its address is in the I/O area, rp2350-io.h),
 * called with interrupts off: it moves to an SRAM stack,
 * parks core 1 in SRAM, runs the bootrom's flash functions, restores the
 * QMI and returns. Linux is frozen meanwhile: one 4 KiB sector erase (about
 * 45 ms) or one 256-byte page program (under 1 ms) per call. The routine
 * itself refuses anything outside its fixed regions.
 *
 * Reads use the uncached, non-allocating XIP alias (0x14000000 up), so they
 * neither see stale cache lines nor push kernel text out of the cache.
 *
 * Each region is an MTD with 16 KiB erase blocks (four sectors) and
 * writesize 1 (bit-writable NOR), the geometry its jffs2 is made for. The
 * "read-only" property, or rp2350_flash.readonly=1 on the command line,
 * makes every region read-only.
 */
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched.h>

#include "rp2350-io.h"
#if IS_ENABLED(CONFIG_RP2350_PIO)
#include "rp2350-pins.h"	/* PIO DMA (into PSRAM) paused */
#endif

#define RPFL_SECTOR	4096
#define RPFL_PAGE	256
#define RPFL_ERASESIZE	16384
#define RPFL_XIP_BASE	0x10000000
#define RPFL_XIP_NOCACHE 0x14000000

static bool readonly;
module_param(readonly, bool, 0444);
MODULE_PARM_DESC(readonly, "never erase or program flash");

/* one routine and one page buffer for every region */
static DEFINE_MUTEX(rpfl_lock);

typedef int (*rpfl_entry_t)(u32 op, u32 addr, u32 len);

struct rpfl {
	struct mtd_info mtd;
	void __iomem *io;
	u32 base;			/* XIP address */
	const u8 __iomem *rd;		/* the region, uncached */
	rpfl_entry_t entry;
};

static int rpfl_call(struct rpfl *f, u32 op, u32 addr)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
#if IS_ENABLED(CONFIG_RP2350_PIO)
	{
		/* a running PIO DMA channel would fault on PSRAM with XIP stopped */
		u32 dma = rp2350_pio_dma_pause();

		ret = f->entry(op, addr, 0);
		rp2350_pio_dma_resume(dma);
	}
#else
	ret = f->entry(op, addr, 0);
#endif
	local_irq_restore(flags);
	return ret;
}

static int rpfl_read(struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf)
{
	struct rpfl *f = mtd->priv;

	memcpy_fromio(buf, f->rd + from, len);
	*retlen = len;
	return 0;
}

static int rpfl_write(struct mtd_info *mtd, loff_t to, size_t len, size_t *retlen, const u_char *buf)
{
	struct rpfl *f = mtd->priv;
	void __iomem *pb = f->io + RPIO_FL_PAGE_BUF;
	int ret = 0;

	*retlen = 0;
	mutex_lock(&rpfl_lock);
	while (len) {
		u32 a = f->base + to, page = a & ~(RPFL_PAGE - 1), off = a - page;
		size_t n = min_t(size_t, RPFL_PAGE - off, len), i;

		for (i = 0; i < n && buf[i] == 0xff; i++)
			;
		if (i < n) {	/* not all 0xff: program the page */
			memset_io(pb, 0xff, RPFL_PAGE);
			memcpy_toio(pb + off, buf, n);
			ret = rpfl_call(f, RPIO_FL_OP_PROGRAM, page);
			if (ret)
				break;
		}
		to += n;
		buf += n;
		len -= n;
		*retlen += n;
		cond_resched();
	}
	mutex_unlock(&rpfl_lock);
	return ret;
}

static int rpfl_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct rpfl *f = mtd->priv;
	u64 a;
	int ret = 0;

	for (a = instr->addr; a < instr->addr + instr->len; a += RPFL_SECTOR) {
		mutex_lock(&rpfl_lock);
		ret = rpfl_call(f, RPIO_FL_OP_ERASE, f->base + a);
		mutex_unlock(&rpfl_lock);
		if (ret) {
			instr->fail_addr = a;
			break;
		}
		cond_resched();
	}
	return ret;
}

/* the routine's counters: erases, pages, longest stall (us), total stall (ms) */
static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rpfl *f = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u %u %u %u\n", readl(f->io + RPIO_FL_ERASES),
			  readl(f->io + RPIO_FL_PAGES), readl(f->io + RPIO_FL_STALL_MAX_US),
			  readl(f->io + RPIO_FL_STALL_TOTAL_MS));
}
static DEVICE_ATTR_RO(stats);

static struct attribute *rpfl_attrs[] = { &dev_attr_stats.attr, NULL };
ATTRIBUTE_GROUPS(rpfl);

static int rpfl_probe(struct platform_device *pdev)
{
	struct resource *r, *io;
	const char *label;
	struct rpfl *f;
	bool ro;

	f = devm_kzalloc(&pdev->dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	io = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!r || !io || r->start % RPFL_ERASESIZE || resource_size(r) % RPFL_ERASESIZE)
		return -EINVAL;
	f->io = devm_ioremap(&pdev->dev, io->start, resource_size(io));
	if (!f->io)
		return -ENOMEM;
	if (readl(f->io) != RPIO_MAGIC || !readl(f->io + RPIO_FL_ENTRY))
		return dev_err_probe(&pdev->dev, -ENODEV, "no flash routine (core 1 firmware too old?)\n");
	f->base = r->start;
	f->rd = (const u8 __iomem *)(uintptr_t)(r->start - RPFL_XIP_BASE + RPFL_XIP_NOCACHE);
	f->entry = (rpfl_entry_t)(uintptr_t)readl(f->io + RPIO_FL_ENTRY);
	ro = readonly || of_property_read_bool(pdev->dev.of_node, "read-only");
	if (of_property_read_string(pdev->dev.of_node, "label", &label))
		label = dev_name(&pdev->dev);

	f->mtd.name = label;
	f->mtd.type = MTD_NORFLASH;
	f->mtd.flags = ro ? 0 : MTD_CAP_NORFLASH;
	f->mtd.size = resource_size(r);
	f->mtd.erasesize = RPFL_ERASESIZE;
	f->mtd.writesize = 1;
	f->mtd.writebufsize = 1;
	f->mtd._read = rpfl_read;
	f->mtd._write = rpfl_write;
	f->mtd._erase = rpfl_erase;
	f->mtd.owner = THIS_MODULE;
	f->mtd.priv = f;
	f->mtd.dev.parent = &pdev->dev;
	mtd_set_of_node(&f->mtd, pdev->dev.of_node);
	platform_set_drvdata(pdev, f);
	dev_info(&pdev->dev, "%s: %u KiB at %#x%s\n", label, (u32)(resource_size(r) >> 10),
		 f->base, ro ? ", read-only" : "");
	return mtd_device_register(&f->mtd, NULL, 0);
}

static const struct of_device_id rpfl_of_match[] = {
	{ .compatible = "raspberrypi,rp2350-flash" },
	{}
};

static struct platform_driver rpfl_driver = {
	.probe = rpfl_probe,
	.driver = {
		.name = "rp2350-flash",
		.of_match_table = rpfl_of_match,
		.suppress_bind_attrs = true,
		.dev_groups = rpfl_groups,
	},
};
builtin_platform_driver(rpfl_driver);
