// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/pio1 and /dev/pio2: the RP2350's PIO1 and PIO2 for programs on core
 * 0, through the interface in rp2350-pio.h (the uapi header user space
 * includes) that libpio and MicroPython's rp2 use. A program maps a block's registers and runs it directly;
 * this driver hands out pins (through gpiolib, so a pin is either a GPIO
 * line or a PIO pin), waits for the block's IRQ0, resets the block between
 * owners, and runs DMA between a buffer it allocated and a state machine's
 * FIFO. PIO0 is core 1's (the Wi-Fi radio).
 *
 * nommu: mmap maps the registers (or the buffer) where they are, through
 * get_unmapped_area. That works for FDPIC processes (which run with the
 * MPU off) as it is; for ATRANS processes, confined to their window by the
 * MPU, rp2350-atrans-dev.c opens the range in spare MPU regions (and holds
 * the file, so release, which frees the buffer, waits for their munmap).
 *
 * A block can drive only the pins whose FUNCSEL is that block's, which
 * only this driver sets. Nothing filters a program's register writes,
 * though: CTRL's NEXTPREV bits and `irq prev/next` reach PIO0.
 *
 * DMA: the chip's channels 12-15, on DMA_IRQ_3. The buffer is kernel memory in PSRAM, through the XIP cache,
 * which the DMA shares with the cores.
 */
#include <linux/bitops.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "rp2350-pins.h"
#include "rp2350-pio.h"

/* the block's registers */
#define PIO_CTRL		0x000
#define PIO_IRQ			0x030
#define PIO_INPUT_SYNC_BYPASS	0x038
#define PIO_INSTR_MEM(n)	(0x048 + 4 * (n))
#define PIO_SM_CLKDIV(n)	(0x0c8 + 0x18 * (n))
#define PIO_SM_EXECCTRL(n)	(0x0cc + 0x18 * (n))
#define PIO_SM_SHIFTCTRL(n)	(0x0d0 + 0x18 * (n))
#define PIO_SM_INSTR(n)		(0x0d8 + 0x18 * (n))
#define PIO_SM_PINCTRL(n)	(0x0dc + 0x18 * (n))
#define PIO_GPIOBASE		0x168
#define PIO_MOV_PINS_NULL	0xa003	/* mov pins, null */
#define PIO_MOV_PINDIRS_NULL	0xa063	/* mov pindirs, null (RP2350) */
#define PIO_IRQ0_INTE		0x170
#define PIO_IRQ0_INTF		0x174
#define PIO_IRQ0_INTS		0x178
#define PIO_IRQ1_INTE		0x17c
#define PIO_IRQ1_INTF		0x180
#define PIO_TXF(n)		(0x010 + 4 * (n))
#define PIO_RXF(n)		(0x020 + 4 * (n))
#define SHIFTCTRL_FJOIN_RX	BIT(31)

#define NPINS			RP_NGPIO

/* the DMA controller: channels DMA_CH0.. are ours */
#define DMA_BASE		((void __iomem *)0x50000000)
#define DMA_CH0			12
#define DMA_NCH			4
#define DMA_REG(c, off)		(DMA_BASE + 0x40 * (DMA_CH0 + (c)) + (off))
#define DMA_READ_ADDR		0x00
#define DMA_WRITE_ADDR		0x04
#define DMA_TRANS_COUNT		0x08
#define DMA_CTRL_TRIG		0x0c
#define DMA_AL1_CTRL		0x10
#define  DMA_EN			BIT(0)
#define  DMA_BUSY		BIT(26)
#define  DMA_AHB_ERROR		BIT(31)
#define  DMA_ENDLESS		(0xfu << 28)	/* TRANS_COUNT's MODE */
#define DMA_INTR		(DMA_BASE + 0x400)
#define DMA_INTE3		(DMA_BASE + 0x434)
#define DMA_INTS3		(DMA_BASE + 0x43c)
#define DMA_CHAN_ABORT		(DMA_BASE + 0x464)
#define DMA_MASK		(((1u << DMA_NCH) - 1) << DMA_CH0)
#define DREQ_PIO0_TX0		0	/* RX FIFOs 4 up, each block 8 up */

struct rp_pio;

static struct rp_dma {
	spinlock_t lock;		/* owner, INTE3 */
	struct rp_pio *owner[DMA_NCH];
	bool from_fifo[DMA_NCH];
	u32 start[DMA_NCH];		/* the memory side's first address */
	wait_queue_head_t wq;
	bool up;
} rp_dma = {
	.lock = __SPIN_LOCK_UNLOCKED(rp_dma.lock),
	.wq = __WAIT_QUEUE_HEAD_INITIALIZER(rp_dma.wq),
};

struct rp_pio {
	struct miscdevice misc;
	char name[8];
	struct device *dev;
	void __iomem *regs;
	phys_addr_t phys;
	unsigned int block;
	u32 clk_hz;
	int irq;
	struct mutex lock;	/* open, pins */
	bool open;
	struct gpio_desc *pins[NPINS];
	wait_queue_head_t wq;
	spinlock_t irq_lock;
	bool irq_off;
	/* the program's DMA buffer */
	struct page *dma_page;
	size_t dma_size;
};

static u64 rp_valid(struct rp_pio *p)
{
	return rp2350_gpio_free_pins();
}

static int rp_claim(struct rp_pio *p, u32 n)
{
	struct gpio_desc *d;

	if (n >= NPINS || !(rp_valid(p) >> n & 1))
		return -EINVAL;
	if (p->pins[n])
		return 0;
	d = gpiod_get_index(p->dev, "pio", n, GPIOD_ASIS);
	if (IS_ERR(d))
		return PTR_ERR(d) == -EPROBE_DEFER ? -EAGAIN : -EBUSY;
	/* gpiolib's request made it a SIO pin with its pad set up */
	rp2350_pin_func(n, RP_FUNC_PIO0 + p->block);
	p->pins[n] = d;
	return 0;
}

static void rp_release_pin(struct rp_pio *p, unsigned int n)
{
	if (!p->pins[n])
		return;
	/* back to its GPIO line, as gpiolib last set it up */
	rp2350_pin_func(n, RP_FUNC_SIO);
	gpiod_put(p->pins[n]);
	p->pins[n] = NULL;
}

/* Back to the state the chip resets to, with every state machine at 0. */
static void rp_reset(struct rp_pio *p)
{
	int k;

	writel(0, p->regs + PIO_IRQ0_INTE);
	writel(0, p->regs + PIO_IRQ1_INTE);
	writel(0, p->regs + PIO_IRQ0_INTF);
	writel(0, p->regs + PIO_IRQ1_INTF);
	/* stop them all, restart their state and clock dividers */
	writel(0xff0, p->regs + PIO_CTRL);
	for (k = 0; k < 4; k++) {
		writel(0x00010000, p->regs + PIO_SM_CLKDIV(k));
		writel(0x0001f000, p->regs + PIO_SM_EXECCTRL(k));
		/* a join change empties both FIFOs */
		writel(0x000c0000 | SHIFTCTRL_FJOIN_RX, p->regs + PIO_SM_SHIFTCTRL(k));
		writel(0x000c0000, p->regs + PIO_SM_SHIFTCTRL(k));
		writel(0x14000000, p->regs + PIO_SM_PINCTRL(k));
		writel(0x0000, p->regs + PIO_SM_INSTR(k)); /* jmp 0 */
	}
	for (k = 0; k < 32; k++)
		writel(0, p->regs + PIO_INSTR_MEM(k));
	writel(0, p->regs + PIO_INPUT_SYNC_BYPASS);
	/*
	 * The block's pin outputs and directions belong to no state machine
	 * and only a block reset clears them: clear them for all 48 pins (32 at
	 * a time from GPIOBASE 16, then 0) from state machine 0. They reach
	 * only pins whose FUNCSEL is this block's.
	 */
	writel(32 << 20, p->regs + PIO_SM_PINCTRL(0)); /* OUT_BASE 0, OUT_COUNT 32 */
	for (k = 16; k >= 0; k -= 16) {
		writel(k, p->regs + PIO_GPIOBASE);
		writel(PIO_MOV_PINS_NULL, p->regs + PIO_SM_INSTR(0));
		writel(PIO_MOV_PINDIRS_NULL, p->regs + PIO_SM_INSTR(0));
	}
	writel(0x14000000, p->regs + PIO_SM_PINCTRL(0));
	writel(0xff, p->regs + PIO_IRQ);
}

static void rp_arm(struct rp_pio *p)
{
	unsigned long flags;

	spin_lock_irqsave(&p->irq_lock, flags);
	if (p->irq_off) {
		p->irq_off = false;
		enable_irq(p->irq);
	}
	spin_unlock_irqrestore(&p->irq_lock, flags);
}

static irqreturn_t rp_irq(int irq, void *data)
{
	struct rp_pio *p = data;

	/* level: off until a waiter has looked (the program clears the cause) */
	spin_lock(&p->irq_lock);
	if (!p->irq_off) {
		p->irq_off = true;
		disable_irq_nosync(irq);
	}
	spin_unlock(&p->irq_lock);
	wake_up_interruptible(&p->wq);
	return IRQ_HANDLED;
}

static struct rp_pio *rp_of(struct file *f)
{
	return container_of(f->private_data, struct rp_pio, misc);
}

/* ------------------------------------------------------------ DMA */

static void rp_dma_hw_stop(unsigned int ch)
{
	u32 bit = BIT(DMA_CH0 + ch), c = readl(DMA_REG(ch, DMA_CTRL_TRIG));
	int k;

	if (c & (DMA_EN | DMA_BUSY)) {
		/* EN off (AL1_CTRL doesn't trigger), then ABORT until it stops */
		writel(c & ~DMA_EN, DMA_REG(ch, DMA_AL1_CTRL));
		writel(bit, DMA_CHAN_ABORT);
		for (k = 0; k < 100000 && (readl(DMA_CHAN_ABORT) & bit); k++)
			cpu_relax();
	}
	writel(bit, DMA_INTR);
}

static void rp_dma_stop(unsigned int ch)
{
	unsigned long flags;

	rp_dma_hw_stop(ch);
	spin_lock_irqsave(&rp_dma.lock, flags);
	writel(BIT(DMA_CH0 + ch), DMA_INTE3 + RP_ALIAS_CLR);
	rp_dma.owner[ch] = NULL;
	spin_unlock_irqrestore(&rp_dma.lock, flags);
	/* a DMA_WAIT on it is over */
	wake_up_interruptible(&rp_dma.wq);
}

u32 rp2350_pio_dma_pause(void)
{
	u32 on = 0;
	int ch, k;

	for (ch = 0; ch < DMA_NCH; ch++) {
		if (readl(DMA_REG(ch, DMA_CTRL_TRIG)) & DMA_EN) {
			on |= BIT(ch);
			writel(DMA_EN, DMA_REG(ch, DMA_AL1_CTRL) + RP_ALIAS_CLR);
		}
	}
	/* the transfers already on the bus finish (a few bus cycles) */
	if (on)
		for (k = 0; k < 2048; k++)
			cpu_relax();
	return on;
}
EXPORT_SYMBOL_GPL(rp2350_pio_dma_pause);

void rp2350_pio_dma_resume(u32 on)
{
	int ch;

	for (ch = 0; ch < DMA_NCH; ch++)
		if (on & BIT(ch))
			writel(DMA_EN, DMA_REG(ch, DMA_AL1_CTRL) + RP_ALIAS_SET);
}
EXPORT_SYMBOL_GPL(rp2350_pio_dma_resume);

/* Every channel of p's, stopped and given back. */
static void rp_dma_release(struct rp_pio *p)
{
	unsigned int ch;

	for (ch = 0; ch < DMA_NCH; ch++)
		if (READ_ONCE(rp_dma.owner[ch]) == p)
			rp_dma_stop(ch);
}

static bool rp_dma_owns(struct rp_pio *p, u32 ch)
{
	return rp_dma.up && ch < DMA_NCH && READ_ONCE(rp_dma.owner[ch]) == p;
}

static int rp_dma_alloc(struct rp_pio *p, u32 size)
{
	if (!rp_dma.up)
		return -ENODEV;
	if (!size || size > RP2350_PIO_DMA_MAX_BUF)
		return -EINVAL;
	if (p->dma_page)
		return -EBUSY;
	size = PAGE_ALIGN(size);
	/* contiguous memory the program asked for: when it isn't there, fail
	 * (ENOMEM; it may ask for less) rather than run the OOM killer, which
	 * an allocation of 32 KiB or less would */
	p->dma_page = alloc_pages(GFP_KERNEL | __GFP_ZERO | __GFP_NORETRY | __GFP_NOWARN, get_order(size));
	if (!p->dma_page)
		return -ENOMEM;
	p->dma_size = size;
	return 0;
}

static void rp_dma_status(unsigned int ch, struct rp2350_pio_dma *d)
{
	u32 c = readl(DMA_REG(ch, DMA_CTRL_TRIG));

	d->busy = !!(c & DMA_BUSY);
	d->error = !!(c & DMA_AHB_ERROR);
	d->remaining = readl(DMA_REG(ch, DMA_TRANS_COUNT)) & 0x0fffffff;
	d->pos = readl(DMA_REG(ch, rp_dma.from_fifo[ch] ? DMA_WRITE_ADDR : DMA_READ_ADDR)) -
		 rp_dma.start[ch];
}

static int rp_dma_start(struct rp_pio *p, struct rp2350_pio_dma *d)
{
	unsigned long flags;
	unsigned int ch, size_log2;
	u32 ctrl, mem, fifo, dreq;
	bool from = d->flags & RP2350_PIO_DMA_FROM_SM;
	u64 span;

	if (!rp_dma.up)
		return -ENODEV;
	if (!p->dma_page)
		return -ENOMEM;
	if (d->sm > 3 || d->flags & ~(RP2350_PIO_DMA_FROM_SM | RP2350_PIO_DMA_ENDLESS) || !d->count ||
	    d->count >= BIT(28))
		return -EINVAL;
	switch (d->size) {
	case 1: size_log2 = 0; break;
	case 2: size_log2 = 1; break;
	case 4: size_log2 = 2; break;
	default: return -EINVAL;
	}
	if (d->ring_bits) {
		if (d->ring_bits > 12 || d->ring_bits < size_log2 || d->offset & (BIT(d->ring_bits) - 1))
			return -EINVAL;
		span = BIT(d->ring_bits);
	} else {
		if (d->flags & RP2350_PIO_DMA_ENDLESS)
			return -EINVAL;
		span = (u64)d->count << size_log2;
	}
	if (d->offset & (d->size - 1) || d->offset + span > p->dma_size)
		return -EINVAL;
	/* a channel: the one named, or a free one */
	spin_lock_irqsave(&rp_dma.lock, flags);
	if (d->channel == RP2350_PIO_DMA_NEW) {
		for (ch = 0; ch < DMA_NCH && rp_dma.owner[ch]; ch++)
			;
		if (ch == DMA_NCH) {
			spin_unlock_irqrestore(&rp_dma.lock, flags);
			return -EBUSY;
		}
		rp_dma.owner[ch] = p;
	} else if (d->channel < DMA_NCH && rp_dma.owner[d->channel] == p) {
		ch = d->channel;
	} else {
		spin_unlock_irqrestore(&rp_dma.lock, flags);
		return -EINVAL;
	}
	writel(BIT(DMA_CH0 + ch), DMA_INTE3 + RP_ALIAS_CLR);
	spin_unlock_irqrestore(&rp_dma.lock, flags);

	rp_dma_hw_stop(ch);
	mem = (u32)(unsigned long)page_address(p->dma_page) + d->offset;
	fifo = (u32)p->phys + (from ? PIO_RXF(d->sm) : PIO_TXF(d->sm));
	dreq = DREQ_PIO0_TX0 + 8 * p->block + (from ? 4 : 0) + d->sm;
	ctrl = DMA_EN | size_log2 << 2 |
	       (from ? BIT(6) : BIT(4)) |		/* INCR_WRITE / INCR_READ */
	       d->ring_bits << 8 |
	       (from ? BIT(12) : 0) |			/* RING_SEL: the write side */
	       (DMA_CH0 + ch) << 13 |			/* CHAIN_TO itself: no chain */
	       dreq << 17;
	rp_dma.from_fifo[ch] = from;
	rp_dma.start[ch] = mem;
	writel(from ? fifo : mem, DMA_REG(ch, DMA_READ_ADDR));
	writel(from ? mem : fifo, DMA_REG(ch, DMA_WRITE_ADDR));
	writel(d->count | (d->flags & RP2350_PIO_DMA_ENDLESS ? DMA_ENDLESS : 0),
	       DMA_REG(ch, DMA_TRANS_COUNT));
	if (!(d->flags & RP2350_PIO_DMA_ENDLESS))
		writel(BIT(DMA_CH0 + ch), DMA_INTE3 + RP_ALIAS_SET);
	writel(ctrl, DMA_REG(ch, DMA_CTRL_TRIG));
	d->channel = ch;
	return 0;
}

/* done, or no longer p's (stopped, or given back on release) */
static bool rp_dma_over(struct rp_pio *p, unsigned int ch)
{
	return !rp_dma_owns(p, ch) || !(readl(DMA_REG(ch, DMA_CTRL_TRIG)) & DMA_BUSY);
}

static long rp_dma_ioctl(struct rp_pio *p, unsigned int cmd, unsigned long arg)
{
	struct rp2350_pio_dma d;
	long ret = 0, t;

	if (copy_from_user(&d, (void __user *)arg, sizeof(d)))
		return -EFAULT;
	if (cmd != RP2350_PIO_DMA_START && !rp_dma_owns(p, d.channel))
		return -EINVAL;
	switch (cmd) {
	case RP2350_PIO_DMA_START:
		mutex_lock(&p->lock);
		ret = rp_dma_start(p, &d);
		mutex_unlock(&p->lock);
		break;
	case RP2350_PIO_DMA_STATUS:
		rp_dma_status(d.channel, &d);
		break;
	case RP2350_PIO_DMA_WAIT:
		t = d.timeout_ms ? msecs_to_jiffies(d.timeout_ms) : MAX_SCHEDULE_TIMEOUT;
		t = wait_event_interruptible_timeout(rp_dma.wq, rp_dma_over(p, d.channel), t);
		if (t < 0)
			return t;
		rp_dma_status(d.channel, &d);
		if (!t && d.busy)
			ret = -ETIMEDOUT;
		break;
	case RP2350_PIO_DMA_STOP:
		/* under p->lock, as START is */
		mutex_lock(&p->lock);
		if (rp_dma_owns(p, d.channel))
			rp_dma_stop(d.channel);
		mutex_unlock(&p->lock);
		return 0;
	}
	if (ret && ret != -ETIMEDOUT)
		return ret;
	if (copy_to_user((void __user *)arg, &d, sizeof(d)))
		return -EFAULT;
	return ret;
}

static irqreturn_t rp_dma_irq(int irq, void *data)
{
	u32 pend = readl(DMA_INTS3) & DMA_MASK;

	if (!pend)
		return IRQ_NONE;
	writel(pend, DMA_INTS3);
	wake_up_interruptible(&rp_dma.wq);
	return IRQ_HANDLED;
}

static int rp2350_pio_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int ch;
	int irq, ret;

	rp2350_unreset(RP_RST_DMA);
	for (ch = 0; ch < DMA_NCH; ch++)
		rp_dma_hw_stop(ch);
	writel(DMA_MASK, DMA_INTE3 + RP_ALIAS_CLR);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, rp_dma_irq, 0, dev_name(dev), &rp_dma);
	if (ret)
		return ret;
	rp_dma.up = true;
	dev_info(dev, "%u channels (%u-%u)\n", DMA_NCH, DMA_CH0, DMA_CH0 + DMA_NCH - 1);
	return 0;
}

/* ------------------------------------------------------------ the device */

static int rp_open(struct inode *inode, struct file *f)
{
	struct rp_pio *p = rp_of(f);
	int ret = 0;

	mutex_lock(&p->lock);
	if (p->open) {
		ret = -EBUSY;
	} else {
		p->open = true;
		rp_reset(p);
	}
	mutex_unlock(&p->lock);
	return ret ? ret : nonseekable_open(inode, f);
}

static int rp_release(struct inode *inode, struct file *f)
{
	struct rp_pio *p = rp_of(f);
	unsigned int n;

	mutex_lock(&p->lock);
	/* the channels first: they feed the block */
	rp_dma_release(p);
	rp_reset(p);
	for (n = 0; n < NPINS; n++)
		rp_release_pin(p, n);
	if (p->dma_page) {
		__free_pages(p->dma_page, get_order(p->dma_size));
		p->dma_page = NULL;
		p->dma_size = 0;
	}
	p->open = false;
	mutex_unlock(&p->lock);
	return 0;
}

static long rp_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct rp_pio *p = rp_of(f);
	struct rp2350_pio_info info = {};
	u32 n;
	long ret;
	unsigned int k;
	u64 valid;

	switch (cmd) {
	case RP2350_PIO_INFO:
		valid = rp_valid(p);
		info.block = p->block;
		info.clk_hz = p->clk_hz;
		info.valid[0] = valid;
		info.valid[1] = valid >> 32;
		mutex_lock(&p->lock);
		for (k = 0; k < NPINS; k++)
			if (p->pins[k])
				info.claimed[k / 32] |= BIT(k % 32);
		mutex_unlock(&p->lock);
		return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
	case RP2350_PIO_PIN_CLAIM:
	case RP2350_PIO_PIN_RELEASE:
		if (get_user(n, (u32 __user *)arg))
			return -EFAULT;
		if (n >= NPINS)
			return -EINVAL;
		mutex_lock(&p->lock);
		ret = 0;
		if (cmd == RP2350_PIO_PIN_CLAIM)
			ret = rp_claim(p, n);
		else
			rp_release_pin(p, n);
		mutex_unlock(&p->lock);
		return ret;
	case RP2350_PIO_DMA_ALLOC:
		if (get_user(n, (u32 __user *)arg))
			return -EFAULT;
		mutex_lock(&p->lock);
		ret = rp_dma_alloc(p, n);
		mutex_unlock(&p->lock);
		return ret;
	case RP2350_PIO_DMA_START:
	case RP2350_PIO_DMA_STATUS:
	case RP2350_PIO_DMA_WAIT:
	case RP2350_PIO_DMA_STOP:
		return rp_dma_ioctl(p, cmd, arg);
	}
	return -ENOTTY;
}

static ssize_t rp_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	struct rp_pio *p = rp_of(f);
	u32 ints;
	int ret;

	if (len < sizeof(ints))
		return -EINVAL;
	for (;;) {
		ints = readl(p->regs + PIO_IRQ0_INTS);
		if (ints)
			break;
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		rp_arm(p);
		ret = wait_event_interruptible(p->wq, readl(p->regs + PIO_IRQ0_INTS));
		if (ret)
			return ret;
	}
	return copy_to_user(buf, &ints, sizeof(ints)) ? -EFAULT : sizeof(ints);
}

static __poll_t rp_poll(struct file *f, poll_table *wait)
{
	struct rp_pio *p = rp_of(f);

	poll_wait(f, &p->wq, wait);
	if (readl(p->regs + PIO_IRQ0_INTS))
		return EPOLLIN | EPOLLRDNORM;
	rp_arm(p);
	return 0;
}

/*
 * nommu: a mapping of this device is direct, at the address this returns
 * (the registers, or the DMA buffer); mmap below then only checks it.
 */
static unsigned long rp_get_unmapped_area(struct file *f, unsigned long addr, unsigned long len,
					  unsigned long pgoff, unsigned long flags)
{
	struct rp_pio *p = rp_of(f);
	unsigned long ret;

	if (pgoff == RP2350_PIO_DMA_BUF_OFFSET >> PAGE_SHIFT) {
		mutex_lock(&p->lock);
		ret = p->dma_page && len <= p->dma_size ?
		      (unsigned long)page_address(p->dma_page) : -EINVAL;
		mutex_unlock(&p->lock);
		return ret;
	}
	if (pgoff || len > RP2350_PIO_MAP_SIZE)
		return -EINVAL;
	return p->phys;
}

static int rp_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct rp_pio *p = rp_of(f);
	unsigned long size = vma->vm_end - vma->vm_start;

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (vma->vm_pgoff == RP2350_PIO_DMA_BUF_OFFSET >> PAGE_SHIFT) {
		/* the DMA buffer */
		if (!p->dma_page || size > p->dma_size)
			return -EINVAL;
		vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
		return remap_pfn_range(vma, vma->vm_start, page_to_pfn(p->dma_page), size,
				       vma->vm_page_prot);
	}
	if (vma->vm_pgoff || size > RP2350_PIO_MAP_SIZE)
		return -EINVAL;
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	return io_remap_pfn_range(vma, vma->vm_start, p->phys >> PAGE_SHIFT, size,
				  vma->vm_page_prot);
}

static unsigned int rp_mmap_capabilities(struct file *f)
{
	return NOMMU_MAP_DIRECT | NOMMU_MAP_READ | NOMMU_MAP_WRITE;
}

static const struct file_operations rp_fops = {
	.owner = THIS_MODULE,
	.open = rp_open,
	.release = rp_release,
	.unlocked_ioctl = rp_ioctl,
	.read = rp_read,
	.poll = rp_poll,
	.mmap = rp_mmap,
	.get_unmapped_area = rp_get_unmapped_area,
	.mmap_capabilities = rp_mmap_capabilities,
};

static int rp2350_pio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rp_pio *p;
	struct resource *r;
	u32 block;
	int ret;

	if (of_property_read_u32(dev->of_node, "raspberrypi,block", &block) || block < 1 || block > 2)
		return -EINVAL;
	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->block = block;
	if (of_property_read_u32(dev->of_node, "clock-frequency", &p->clk_hz))
		return -EINVAL;
	p->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &r);
	if (IS_ERR(p->regs))
		return PTR_ERR(p->regs);
	p->phys = r->start;
	if (resource_size(r) < RP2350_PIO_MAP_SIZE)
		return -EINVAL;
	mutex_init(&p->lock);
	spin_lock_init(&p->irq_lock);
	init_waitqueue_head(&p->wq);
	rp2350_unreset(block == 1 ? RP_RST_PIO1 : RP_RST_PIO2);
	rp_reset(p);
	p->irq = platform_get_irq(pdev, 0);
	if (p->irq < 0)
		return p->irq;
	ret = devm_request_irq(dev, p->irq, rp_irq, 0, dev_name(dev), p);
	if (ret)
		return ret;
	snprintf(p->name, sizeof(p->name), "pio%u", block);
	p->misc.minor = MISC_DYNAMIC_MINOR;
	p->misc.name = p->name;
	p->misc.fops = &rp_fops;
	p->misc.mode = 0600;
	p->misc.parent = dev;
	ret = misc_register(&p->misc);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, p);
	dev_info(dev, "PIO%u, %u MHz\n", block, p->clk_hz / 1000000);
	return 0;
}

static const struct of_device_id rp2350_pio_of[] = {
	{ .compatible = "raspberrypi,rp2350-pio" },
	{}
};

static struct platform_driver rp2350_pio_driver = {
	.probe = rp2350_pio_probe,
	.driver = {
		.name = "rp2350-pio",
		.of_match_table = rp2350_pio_of,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rp2350_pio_driver);

static const struct of_device_id rp2350_pio_dma_of[] = {
	{ .compatible = "raspberrypi,rp2350-pio-dma" },
	{}
};

static struct platform_driver rp2350_pio_dma_driver = {
	.probe = rp2350_pio_dma_probe,
	.driver = {
		.name = "rp2350-pio-dma",
		.of_match_table = rp2350_pio_dma_of,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rp2350_pio_dma_driver);

MODULE_LICENSE("GPL");
