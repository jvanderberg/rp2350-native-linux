// SPDX-License-Identifier: GPL-2.0
/*
 * Raspberry Pi RP2350 (Cortex-M33 cores), device tree machine.
 */
#include <asm/mach/arch.h>
#include <asm/v7m.h>

static const char *const rp2350_compat[] __initconst = {
	"raspberrypi,rp2350",
	NULL
};

DT_MACHINE_START(RP2350DT, "RP2350 (Device Tree Support)")
	.dt_compat = rp2350_compat,
	.restart = armv7m_restart,
MACHINE_END
