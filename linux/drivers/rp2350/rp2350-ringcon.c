// SPDX-License-Identifier: GPL-2.0
/*
 * RP2350 ring console: a tty and console over two byte rings in SRAM. The
 * firmware on core 1 moves the bytes to and from a USB CDC-ACM port.
 *
 * Shared layout (little endian u32s at the start of the region):
 *   0x00 magic "RCON"   0x04 out_size   0x08 in_size   0x0c flags
 *   0x10 out_head (core 1 consumes)     0x14 out_tail (Linux produces)
 *   0x18 in_head  (Linux consumes)      0x1c in_tail  (core 1 produces)
 *   in ring at +0x100, out ring at +0x1000; sizes are powers of two and
 *   the indices run free (index & (size - 1) is the offset).
 * flags bit 0: a terminal is open (DTR). Core 1 drops the oldest output
 * while none is, so a writer never waits for long.
 *
 * Input is polled (core 1 raises no interrupt for it): every POLL_MS, and
 * after output when the ring was full.
 */
#include <linux/console.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>

#define RCON_MAGIC	0x4e4f4352
#define R_MAGIC		0x00
#define R_OUT_SIZE	0x04
#define R_IN_SIZE	0x08
#define R_OUT_HEAD	0x10
#define R_OUT_TAIL	0x14
#define R_IN_HEAD	0x18
#define R_IN_TAIL	0x1c
#define R_IN_BUF	0x100
#define R_OUT_BUF	0x1000

#define POLL_MS		10

struct rcon {
	void __iomem *base;
	u32 out_size, in_size;
	spinlock_t lock;		/* producers of the out ring */
	struct tty_port port;
	struct timer_list poll;
	struct tty_driver *driver;
};

static struct rcon rcon;
static void __iomem *early_base;

/* Copy up to n bytes into the out ring at base; returns how many fit. */
static unsigned int rcon_put(void __iomem *b, const u8 *s, unsigned int n)
{
	u32 size = readl_relaxed(b + R_OUT_SIZE);
	u32 head = readl(b + R_OUT_HEAD);
	u32 tail = readl_relaxed(b + R_OUT_TAIL);
	unsigned int room = size - (tail - head), i;

	if (n > room)
		n = room;
	for (i = 0; i < n; i++)
		writeb_relaxed(s[i], b + R_OUT_BUF + ((tail + i) & (size - 1)));
	writel(tail + n, b + R_OUT_TAIL);	/* writel orders the bytes first */
	return n;
}

static unsigned int rcon_room(void __iomem *b)
{
	u32 size = readl_relaxed(b + R_OUT_SIZE);

	return size - (readl_relaxed(b + R_OUT_TAIL) - readl(b + R_OUT_HEAD));
}

/* Console output: blocks (spinning) until it fits; LF becomes CR LF. */
static void rcon_put_all(void __iomem *b, const char *s, unsigned int n)
{
	static const u8 crlf[2] = { '\r', '\n' };
	unsigned int i, start = 0;

	for (i = 0; i <= n; i++) {
		if (i < n && s[i] != '\n')
			continue;
		while (start < i)
			start += rcon_put(b, (const u8 *)s + start, i - start);
		if (i < n) {
			unsigned int k = 0;

			while (k < 2)
				k += rcon_put(b, crlf + k, 2 - k);
		}
		start = i + 1;
	}
}

static void rcon_console_write(struct console *co, const char *s, unsigned int n)
{
	unsigned long flags;

	spin_lock_irqsave(&rcon.lock, flags);
	rcon_put_all(rcon.base, s, n);
	spin_unlock_irqrestore(&rcon.lock, flags);
}

static struct tty_driver *rcon_console_device(struct console *co, int *index)
{
	*index = 0;
	return rcon.driver;
}

static struct console rcon_console = {
	.name = "ttyRC",
	.write = rcon_console_write,
	.device = rcon_console_device,
	.flags = CON_PRINTBUFFER,
	.index = -1,
};

static void rcon_poll(struct timer_list *t)
{
	void __iomem *b = rcon.base;
	u32 head = readl_relaxed(b + R_IN_HEAD);
	u32 tail = readl(b + R_IN_TAIL);
	bool got = false;

	while (head != tail) {
		u8 c = readb_relaxed(b + R_IN_BUF + (head & (rcon.in_size - 1)));

		if (!tty_insert_flip_char(&rcon.port, c, TTY_NORMAL))
			break;
		head++;
		got = true;
	}
	if (got) {
		writel(head, b + R_IN_HEAD);
		tty_flip_buffer_push(&rcon.port);
	}
	if (rcon_room(b))
		tty_port_tty_wakeup(&rcon.port);
	mod_timer(&rcon.poll, jiffies + msecs_to_jiffies(POLL_MS));
}

static ssize_t rcon_write(struct tty_struct *tty, const u8 *buf, size_t count)
{
	unsigned long flags;
	unsigned int n;

	spin_lock_irqsave(&rcon.lock, flags);
	n = rcon_put(rcon.base, buf, count);
	spin_unlock_irqrestore(&rcon.lock, flags);
	return n;
}

static unsigned int rcon_write_room(struct tty_struct *tty)
{
	return rcon_room(rcon.base);
}

static int rcon_open(struct tty_struct *tty, struct file *filp)
{
	return tty_port_open(&rcon.port, tty, filp);
}

static void rcon_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(&rcon.port, tty, filp);
}

static void rcon_hangup(struct tty_struct *tty)
{
	tty_port_hangup(&rcon.port);
}

static int rcon_install(struct tty_driver *driver, struct tty_struct *tty)
{
	return tty_port_install(&rcon.port, driver, tty);
}

static const struct tty_operations rcon_ops = {
	.install = rcon_install,
	.open = rcon_open,
	.close = rcon_close,
	.hangup = rcon_hangup,
	.write = rcon_write,
	.write_room = rcon_write_room,
};

static const struct tty_port_operations rcon_port_ops = {};

static int rcon_probe(struct platform_device *pdev)
{
	struct tty_driver *drv;
	struct device *dev;
	int ret;

	rcon.base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rcon.base))
		return PTR_ERR(rcon.base);
	if (readl(rcon.base + R_MAGIC) != RCON_MAGIC)
		return dev_err_probe(&pdev->dev, -ENODEV, "no ring header (core 1 firmware missing?)\n");
	rcon.out_size = readl(rcon.base + R_OUT_SIZE);
	rcon.in_size = readl(rcon.base + R_IN_SIZE);
	spin_lock_init(&rcon.lock);
	tty_port_init(&rcon.port);
	rcon.port.ops = &rcon_port_ops;

	drv = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(drv))
		return PTR_ERR(drv);
	drv->driver_name = "rp2350-ringcon";
	drv->name = "ttyRC";
	drv->type = TTY_DRIVER_TYPE_SERIAL;
	drv->subtype = SERIAL_TYPE_NORMAL;
	drv->init_termios = tty_std_termios;
	tty_set_operations(drv, &rcon_ops);
	ret = tty_register_driver(drv);
	if (ret) {
		tty_driver_kref_put(drv);
		return ret;
	}
	rcon.driver = drv;
	dev = tty_port_register_device(&rcon.port, drv, 0, &pdev->dev);
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	timer_setup(&rcon.poll, rcon_poll, 0);
	mod_timer(&rcon.poll, jiffies + 1);
	register_console(&rcon_console);
	dev_info(&pdev->dev, "ring console: out %u, in %u bytes\n", rcon.out_size, rcon.in_size);
	return 0;
}

static const struct of_device_id rcon_of_match[] = {
	{ .compatible = "raspberrypi,rp2350-ringcon" },
	{}
};

static struct platform_driver rcon_platform_driver = {
	.probe = rcon_probe,
	.driver = {
		.name = "rp2350-ringcon",
		.of_match_table = rcon_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rcon_platform_driver);

/* earlycon=rp2350ring,0x20080000 or via stdout-path */
static void rcon_early_write(struct console *con, const char *s, unsigned int n)
{
	rcon_put_all(early_base, s, n);
}

static int __init rcon_early_setup(struct earlycon_device *dev, const char *opt)
{
	if (!dev->port.membase || readl(dev->port.membase + R_MAGIC) != RCON_MAGIC)
		return -ENODEV;
	early_base = dev->port.membase;
	dev->con->write = rcon_early_write;
	return 0;
}
OF_EARLYCON_DECLARE(rp2350ring, "raspberrypi,rp2350-ringcon", rcon_early_setup);
