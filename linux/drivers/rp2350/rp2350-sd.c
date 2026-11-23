// SPDX-License-Identifier: GPL-2.0
/*
 * RP2350 SD card: a block device (mmcblk0) whose requests core 1 carries out
 * on the card (boot/src/sd.rs, over SPI1 with the boot/sdcard crate:
 * CRCs, retries, insertion and removal). There is no MMC
 * layer: the SD protocol runs on core 1.
 *
 * One request at a time, through the SD card control block of the I/O area
 * (rp2350-io.h): its sector, its direction and up to RPIO_BLK_MAX_SEGS
 * buffers in RAM. Core 1 sends a write straight from them (PSRAM, through
 * the XIP cache both cores share), but never writes them: the blocks of a
 * read come through SRAM slots, and the interrupt copies each one out
 * (rpsd_copy). Core 0 cleans and invalidates the XIP cache by set/way, two
 * operations per line (rp2350-atrans.c), so a line core 1 dirtied between
 * the two would be lost; with only core 0 writing PSRAM, nothing can be.
 * Each copy is checked against the block's CRC16 (stats: mismatch, which
 * should stay 0). Core 1 rings RPIO_BELL_BLK for each block, when it is done,
 * and when the card changed (the size is 0 while there is none); a change
 * updates the capacity (a uevent) and marks the media changed. Partitions
 * are reread by userspace (S03sd: partprobe).
 */
#include <linux/blk-mq.h>
#include <linux/crc-itu-t.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include "rp2350-io.h"

#define RPSD_MINORS	16
#define RPSD_MAX_BYTES	(64 * 1024)

struct rpsd {
	void __iomem *io;
	struct device *dev;
	struct blk_mq_tag_set tag_set;
	struct gendisk *disk;
	spinlock_t lock;
	struct request *cur;		/* the request core 1 has */
	u32 req;			/* its number */
	u32 gen;			/* the card generation last seen */
	struct work_struct change;
	/* the request's buffers, where read blocks may go */
	u32 seg_addr[RPIO_BLK_MAX_SEGS], seg_len[RPIO_BLK_MAX_SEGS];
	u32 nseg;
	u32 copied;			/* read blocks copied out (RD_COPIED) */
	u32 mismatch, bad_dest;		/* copies off their CRC; stray slots */
};

static int rpsd_major;

static u64 rpsd_sectors(struct rpsd *s)
{
	u32 hi, lo;

	do {
		hi = readl(s->io + RPIO_BLK_SECTORS_HI);
		lo = readl(s->io + RPIO_BLK_SECTORS_LO);
	} while (hi != readl(s->io + RPIO_BLK_SECTORS_HI));
	return (u64)hi << 32 | lo;
}

static blk_status_t rpsd_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
	struct rpsd *s = hctx->queue->queuedata;
	struct request *rq = bd->rq;
	struct req_iterator iter;
	struct bio_vec bv;
	unsigned long flags;
	u32 op, n = 0, last_addr = 0, last_len = 0;

	blk_mq_start_request(rq);
	switch (req_op(rq)) {
	case REQ_OP_READ:
		op = RPIO_BLK_OP_READ;
		break;
	case REQ_OP_WRITE:
		op = RPIO_BLK_OP_WRITE;
		break;
	default:
		return BLK_STS_IOERR;
	}
	if (blk_rq_pos(rq) + blk_rq_sectors(rq) > get_capacity(s->disk))
		return BLK_STS_IOERR;
	/* physically contiguous buffers become one segment */
	rq_for_each_segment(bv, rq, iter) {
		u32 a = (u32)bvec_virt(&bv);

		if (n && last_addr + last_len == a) {
			last_len += bv.bv_len;
			writel(last_len, s->io + RPIO_BLK_SEGS + 8 * (n - 1) + 4);
			continue;
		}
		if (n == RPIO_BLK_MAX_SEGS)
			return BLK_STS_IOERR;
		writel(a, s->io + RPIO_BLK_SEGS + 8 * n);
		writel(bv.bv_len, s->io + RPIO_BLK_SEGS + 8 * n + 4);
		s->seg_addr[n] = a;
		last_addr = a;
		last_len = bv.bv_len;
		n++;
	}
	if (!n)
		return BLK_STS_IOERR;
	for (u32 i = 0; i < n; i++)
		s->seg_len[i] = readl(s->io + RPIO_BLK_SEGS + 8 * i + 4);
	s->nseg = n;
	writel(op, s->io + RPIO_BLK_OP);
	writel(n, s->io + RPIO_BLK_NSEG);
	writel(lower_32_bits(blk_rq_pos(rq)), s->io + RPIO_BLK_SECTOR_LO);
	writel(upper_32_bits(blk_rq_pos(rq)), s->io + RPIO_BLK_SECTOR_HI);
	spin_lock_irqsave(&s->lock, flags);
	s->cur = rq;
	s->req++;
	wmb();
	writel(s->req, s->io + RPIO_BLK_REQ);
	spin_unlock_irqrestore(&s->lock, flags);
	rpio_ring_core1(RPIO_BELL1_BLK);
	return BLK_STS_OK;
}

static const struct blk_mq_ops rpsd_mq_ops = {
	.queue_rq = rpsd_queue_rq,
};

static void rpsd_change(struct work_struct *w)
{
	struct rpsd *s = container_of(w, struct rpsd, change);
	u64 n = rpsd_sectors(s);

	dev_info(s->dev, "card %s (%llu sectors)\n", n ? "in" : "out", n);
	disk_force_media_change(s->disk);
	set_capacity_and_notify(s->disk, n);
}

/* the block in a slot is for one of the current request's buffers */
static bool rpsd_dest_ok(struct rpsd *s, u32 a)
{
	for (u32 i = 0; i < s->nseg; i++)
		if (a >= s->seg_addr[i] && a - s->seg_addr[i] <= s->seg_len[i] - RPIO_BLK_RD_SLOT_LEN)
			return true;
	return false;
}

/* Copy out the read blocks core 1 has handed over (s->lock held). */
static void rpsd_copy(struct rpsd *s)
{
	u32 filled = readl(s->io + RPIO_BLK_RD_FILLED);

	if (filled == s->copied)
		return;
	rmb();
	while (s->copied != filled) {
		u32 k = s->copied % RPIO_BLK_RD_SLOTS;
		u32 a = readl(s->io + RPIO_BLK_RD_DEST + 4 * k);
		u16 crc = readl(s->io + RPIO_BLK_RD_CRC + 4 * k);
		void __iomem *slot = s->io + RPIO_BLK_RD_SLOT + RPIO_BLK_RD_SLOT_LEN * k;

		if (!s->cur || !rpsd_dest_ok(s, a)) {
			s->bad_dest++;
		} else {
			memcpy_fromio((void *)a, slot, RPIO_BLK_RD_SLOT_LEN);
			if (crc_itu_t(0, (void *)a, RPIO_BLK_RD_SLOT_LEN) != crc) {
				s->mismatch++;
				memcpy_fromio((void *)a, slot, RPIO_BLK_RD_SLOT_LEN);
			}
		}
		s->copied++;
	}
	mb();
	writel(s->copied, s->io + RPIO_BLK_RD_COPIED);
}

static irqreturn_t rpsd_irq(int irq, void *data)
{
	struct rpsd *s = data;
	struct request *rq = NULL;
	blk_status_t st = BLK_STS_OK;
	u32 gen;

	if (!rpio_take_bell(RPIO_BELL_BLK))
		return IRQ_NONE;
	spin_lock(&s->lock);
	rpsd_copy(s);
	if (s->cur && readl(s->io + RPIO_BLK_DONE) == s->req) {
		rmb();
		rpsd_copy(s);	/* core 1 hands the last blocks over before DONE */
		rq = s->cur;
		s->cur = NULL;
		if (readl(s->io + RPIO_BLK_STATUS))
			st = BLK_STS_IOERR;
	}
	spin_unlock(&s->lock);
	if (rq)
		blk_mq_end_request(rq, st);
	gen = readl(s->io + RPIO_BLK_GEN);
	if (gen != s->gen) {
		s->gen = gen;
		schedule_work(&s->change);
	}
	return IRQ_HANDLED;
}

static const struct block_device_operations rpsd_fops = {
	.owner = THIS_MODULE,
};

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rpsd *s = dev_get_drvdata(dev);

	return sysfs_emit(buf, "reads %u writes %u errors %u clock %u busy_us %u bytes %u xfer_us %u\n"
			  "crc %u rejects %u timeouts %u failed %u inits %u busy_max_us %u last_error %#x bad %u\n"
			  "slot_waits %u mismatch %u bad_dest %u\n",
			  readl(s->io + RPIO_BLK_READS), readl(s->io + RPIO_BLK_WRITES),
			  readl(s->io + RPIO_BLK_ERRORS), readl(s->io + RPIO_BLK_CLOCK),
			  readl(s->io + RPIO_BLK_BUSY_US), readl(s->io + RPIO_BLK_BYTES),
			  readl(s->io + RPIO_DBG_SD_XFER_US),
			  readl(s->io + RPIO_DBG_SD_CRC), readl(s->io + RPIO_DBG_SD_REJECTS),
			  readl(s->io + RPIO_DBG_SD_TIMEOUTS), readl(s->io + RPIO_DBG_SD_FAILED),
			  readl(s->io + RPIO_DBG_SD_INITS), readl(s->io + RPIO_DBG_SD_BUSY_MAX_US),
			  readl(s->io + RPIO_DBG_SD_LAST_ERR), readl(s->io + RPIO_DBG_SD_BAD_REQ),
			  readl(s->io + RPIO_DBG_SD_SLOT_WAITS), s->mismatch, s->bad_dest);
}
static DEVICE_ATTR_RO(stats);

static struct attribute *rpsd_attrs[] = { &dev_attr_stats.attr, NULL };
ATTRIBUTE_GROUPS(rpsd);

static int rpsd_probe(struct platform_device *pdev)
{
	struct queue_limits lim = {
		.logical_block_size = 512,
		.max_hw_sectors = RPSD_MAX_BYTES >> SECTOR_SHIFT,
		.max_segments = RPIO_BLK_MAX_SEGS,
		.max_segment_size = RPSD_MAX_BYTES,
	};
	struct resource *res;
	struct rpsd *s;
	int irq, ret;

	s = devm_kzalloc(&pdev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = &pdev->dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	s->io = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!s->io)
		return -ENOMEM;
	if (readl(s->io) != RPIO_MAGIC)
		return dev_err_probe(&pdev->dev, -ENODEV, "no I/O area (core 1 firmware too old?)\n");
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	spin_lock_init(&s->lock);
	INIT_WORK(&s->change, rpsd_change);
	s->req = readl(s->io + RPIO_BLK_DONE);
	s->gen = readl(s->io + RPIO_BLK_GEN);
	/* read blocks through the slots from now on */
	s->copied = readl(s->io + RPIO_BLK_RD_FILLED);
	writel(s->copied, s->io + RPIO_BLK_RD_COPIED);
	wmb();
	writel(1, s->io + RPIO_BLK_RD_ON);
	platform_set_drvdata(pdev, s);

	ret = register_blkdev(0, "rpsd");
	if (ret < 0)
		return ret;
	rpsd_major = ret;
	s->tag_set.ops = &rpsd_mq_ops;
	s->tag_set.nr_hw_queues = 1;
	s->tag_set.queue_depth = 1;
	s->tag_set.numa_node = NUMA_NO_NODE;
	s->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
	ret = blk_mq_alloc_tag_set(&s->tag_set);
	if (ret)
		goto err_blkdev;
	s->disk = blk_mq_alloc_disk(&s->tag_set, &lim, s);
	if (IS_ERR(s->disk)) {
		ret = PTR_ERR(s->disk);
		goto err_tags;
	}
	s->disk->major = rpsd_major;
	s->disk->first_minor = 0;
	s->disk->minors = RPSD_MINORS;
	s->disk->fops = &rpsd_fops;
	s->disk->private_data = s;
	strscpy(s->disk->disk_name, "mmcblk0", DISK_NAME_LEN);
	set_capacity(s->disk, rpsd_sectors(s));
	ret = devm_request_irq(&pdev->dev, irq, rpsd_irq, IRQF_SHARED, "rp2350-sd", s);
	if (ret)
		goto err_disk;
	ret = add_disk(s->disk);
	if (ret)
		goto err_disk;
	dev_info(&pdev->dev, "SD card via core 1: %llu sectors\n", rpsd_sectors(s));
	return 0;

err_disk:
	put_disk(s->disk);
err_tags:
	blk_mq_free_tag_set(&s->tag_set);
err_blkdev:
	unregister_blkdev(rpsd_major, "rpsd");
	return ret;
}

static const struct of_device_id rpsd_of_match[] = {
	{ .compatible = "raspberrypi,rp2350-sd" },
	{}
};

static struct platform_driver rpsd_driver = {
	.probe = rpsd_probe,
	.driver = {
		.name = "rp2350-sd",
		.of_match_table = rpsd_of_match,
		.suppress_bind_attrs = true,
		.dev_groups = rpsd_groups,
	},
};
builtin_platform_driver(rpsd_driver);
