// SPDX-License-Identifier: GPL-2.0
/*
 * Raspberry Pi RP2350 (Cortex-M33 cores), device tree machine.
 */
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <asm/mach/arch.h>
#include <asm/v7m.h>

#define RP_PSM_WDSEL		((void __iomem *)0x40018008)
#define RP_WATCHDOG_CTRL	((void __iomem *)0x400d8000)
#define RP_WATCHDOG_LOAD	((void __iomem *)0x400d8004)
#define WATCHDOG_ENABLE		BIT(30)
#define WATCHDOG_LOAD_MAX	0x00ffffff
/*
 * What one boot tells the next, in POWMAN's scratch registers (boot/src/boot.rs
 * has the same values): SCRATCH0 counts boots that didn't reach init
 * (BOOTS_MAGIC | n), SCRATCH1 holds a request for the next boot.
 */
#define RP_POWMAN_SCRATCH0	((void __iomem *)0x401000b0)
#define RP_POWMAN_SCRATCH1	((void __iomem *)0x401000b4)
#define BOOTS_MAGIC		0xb0070000
#define REQ_SETUP		0x53455455
/* every PSM domain but the ring and crystal oscillators (bits 2, 3) */
#define WDSEL_ALL_BUT_OSC	(GENMASK(24, 0) & ~(BIT(2) | BIT(3)))
#define WATCHDOG_TRIGGER	BIT(31)
/* the console ring (rp2350-ringcon.c): core 1 consumes at out_head */
#define RP_RCON_OUT_HEAD	((void __iomem *)0x20080010)
#define RP_RCON_OUT_TAIL	((void __iomem *)0x20080014)

/*
 * Restart: a whole-chip reset through the watchdog, as the Pico SDK's
 * watchdog_reboot does: both cores and every peripheral but the
 * oscillators, then the bootrom and the boot stub as at power-on.
 * SYSRESETREQ (armv7m_restart) doesn't reset what core 1 runs (USB, PIO0,
 * SPI1, the SIO), so the next boot could start with them busy.
 */
static void rp2350_restart(enum reboot_mode mode, const char *cmd)
{
	int ms;

	/* reboot(2) with RESTART2 "config": the boot stub's setup screen next */
	if (cmd && !strcmp(cmd, "config"))
		writel(REQ_SETUP, RP_POWMAN_SCRATCH1);

	/* the reset stops core 1 too: let it send the console's last lines
	 * ("reboot: Restarting system") first, for up to 100 ms (it may have
	 * no USB host to send them to) */
	for (ms = 0; ms < 100 && readl(RP_RCON_OUT_HEAD) != readl(RP_RCON_OUT_TAIL); ms++)
		mdelay(1);
	writel(WDSEL_ALL_BUT_OSC, RP_PSM_WDSEL);
	writel(WATCHDOG_TRIGGER, RP_WATCHDOG_CTRL);
	for (;;)
		wfi();
}

/*
 * The boot watchdog. The boot stub starts the watchdog (about 16.7 s) just
 * before it starts the kernel, when the device tree has
 * /chosen/rp2350,boot-watchdog. It is fed here from early on, by a timer,
 * until /sbin/overlay-init writes /proc/rp2350/boot_ok; then it is stopped
 * and the stub's count of boots that didn't get that far is zeroed. The
 * timer feeds it for BOOT_WD_FEEDS seconds at most, so a boot that never
 * gets there resets the board too, not only a hang with interrupts off or
 * a panic; after two such boots in a row the stub opens its setup screen
 * instead of booting.
 */
#define BOOT_WD_FEEDS		120
static struct timer_list boot_wd;
static bool boot_wd_on;
static unsigned int boot_wd_fed;

static void boot_wd_feed(struct timer_list *t)
{
	writel(WATCHDOG_LOAD_MAX, RP_WATCHDOG_LOAD);
	if (++boot_wd_fed < BOOT_WD_FEEDS)
		mod_timer(t, jiffies + HZ);
	else
		pr_warn("rp2350: no boot_ok in %u s; the watchdog will reset the board\n",
			BOOT_WD_FEEDS);
}

static ssize_t boot_ok_write(struct file *f, const char __user *buf, size_t n, loff_t *pos)
{
	if (boot_wd_on) {
		timer_delete_sync(&boot_wd);
		writel(readl(RP_WATCHDOG_CTRL) & ~WATCHDOG_ENABLE, RP_WATCHDOG_CTRL);
		boot_wd_on = false;
	}
	writel(BOOTS_MAGIC, RP_POWMAN_SCRATCH0);
	return n;
}

static int boot_ok_show(struct seq_file *m, void *v)
{
	u32 b = readl(RP_POWMAN_SCRATCH0);

	seq_printf(m, "%u\n", (b & 0xffff0000) == BOOTS_MAGIC ? b & 0xffff : 0);
	return 0;
}

static int boot_ok_open(struct inode *inode, struct file *f)
{
	return single_open(f, boot_ok_show, NULL);
}

static const struct proc_ops boot_ok_ops = {
	.proc_open = boot_ok_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = boot_ok_write,
};

/* "config": the next boot opens the stub's setup screen (`reboot config`) */
static ssize_t next_boot_write(struct file *f, const char __user *buf, size_t n, loff_t *pos)
{
	char k[16];
	size_t len = min(n, sizeof(k) - 1);

	if (copy_from_user(k, buf, len))
		return -EFAULT;
	k[len] = 0;
	strim(k);
	if (!strcmp(k, "config"))
		writel(REQ_SETUP, RP_POWMAN_SCRATCH1);
	else if (!strcmp(k, "normal"))
		writel(0, RP_POWMAN_SCRATCH1);
	else
		return -EINVAL;
	return n;
}

static int next_boot_show(struct seq_file *m, void *v)
{
	seq_puts(m, readl(RP_POWMAN_SCRATCH1) == REQ_SETUP ? "config\n" : "normal\n");
	return 0;
}

static int next_boot_open(struct inode *inode, struct file *f)
{
	return single_open(f, next_boot_show, NULL);
}

static const struct proc_ops next_boot_ops = {
	.proc_open = next_boot_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = next_boot_write,
};

static int __init rp2350_boot_init(void)
{
	struct proc_dir_entry *d;

	if (readl(RP_WATCHDOG_CTRL) & WATCHDOG_ENABLE) {
		boot_wd_on = true;
		timer_setup(&boot_wd, boot_wd_feed, 0);
		boot_wd_feed(&boot_wd);
	}
	d = proc_mkdir("rp2350", NULL);
	if (!d)
		return -ENOMEM;
	proc_create("boot_ok", 0600, d, &boot_ok_ops);
	proc_create("next_boot", 0600, d, &next_boot_ops);
	return 0;
}
core_initcall(rp2350_boot_init);

static const char *const rp2350_compat[] __initconst = {
	"raspberrypi,rp2350",
	NULL
};

DT_MACHINE_START(RP2350DT, "RP2350 (Device Tree Support)")
	.dt_compat = rp2350_compat,
	.restart = rp2350_restart,
MACHINE_END
