// SPDX-License-Identifier: GPL-2.0
/*
 * Raspberry Pi RP2350 (Cortex-M33 cores), device tree machine.
 */
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/reboot.h>
#include <asm/mach/arch.h>
#include <asm/v7m.h>

#define RP_PSM_WDSEL		((void __iomem *)0x40018008)
#define RP_WATCHDOG_CTRL	((void __iomem *)0x400d8000)
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

static const char *const rp2350_compat[] __initconst = {
	"raspberrypi,rp2350",
	NULL
};

DT_MACHINE_START(RP2350DT, "RP2350 (Device Tree Support)")
	.dt_compat = rp2350_compat,
	.restart = rp2350_restart,
MACHINE_END
