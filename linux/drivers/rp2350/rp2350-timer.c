// SPDX-License-Identifier: GPL-2.0
/*
 * RP2350 TIMER0: a 64-bit microsecond counter (ticked at 1 MHz by the
 * TICKS block, set up by the boot stub) with four 32-bit alarms.
 * Clocksource and sched_clock: TIMERAWL. Clockevent: ALARM0, oneshot.
 *
 * An alarm fires when the low 32 bits of the counter equal its value, so
 * a target already in the past would wait a full wrap (71 minutes):
 * set_next_event checks for that and returns -ETIME.
 */
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>

#define ALARM0		0x10
#define ARMED		0x20
#define TIMERAWL	0x28
#define DBGPAUSE	0x2c
#define INTR		0x3c
#define INTE		0x40

#define RATE		1000000

static void __iomem *base;

static u64 notrace rp2350_sched_read(void)
{
	return readl_relaxed(base + TIMERAWL);
}

static int rp2350_set_next_event(unsigned long delta, struct clock_event_device *ce)
{
	u32 target = readl_relaxed(base + TIMERAWL) + delta;

	writel_relaxed(target, base + ALARM0);
	if ((s32)(readl_relaxed(base + TIMERAWL) - target) >= 0 &&
	    (readl_relaxed(base + ARMED) & 1)) {
		writel_relaxed(1, base + ARMED);
		return -ETIME;
	}
	return 0;
}

static int rp2350_shutdown(struct clock_event_device *ce)
{
	writel_relaxed(1, base + ARMED);
	return 0;
}

static struct clock_event_device rp2350_ce = {
	.name = "rp2350-alarm0",
	.rating = 300,
	.features = CLOCK_EVT_FEAT_ONESHOT,
	.set_next_event = rp2350_set_next_event,
	.set_state_shutdown = rp2350_shutdown,
	.set_state_oneshot = rp2350_shutdown,
};

static irqreturn_t rp2350_timer_irq(int irq, void *dev_id)
{
	writel_relaxed(1, base + INTR);
	rp2350_ce.event_handler(&rp2350_ce);
	return IRQ_HANDLED;
}

static int __init rp2350_timer_init(struct device_node *np)
{
	int irq, ret;

	base = of_iomap(np, 0);
	if (!base)
		return -ENXIO;
	irq = irq_of_parse_and_map(np, 0);
	if (!irq)
		return -EINVAL;

	writel_relaxed(0, base + DBGPAUSE);
	writel_relaxed(1, base + ARMED);
	writel_relaxed(1, base + INTR);
	writel_relaxed(1, base + INTE);

	ret = clocksource_mmio_init(base + TIMERAWL, "rp2350-timer", RATE, 300, 32,
				    clocksource_mmio_readl_up);
	if (ret)
		return ret;
	sched_clock_register(rp2350_sched_read, 32, RATE);

	ret = request_irq(irq, rp2350_timer_irq, IRQF_TIMER, "rp2350-timer", NULL);
	if (ret)
		return ret;
	rp2350_ce.irq = irq;
	rp2350_ce.cpumask = cpu_possible_mask;
	clockevents_config_and_register(&rp2350_ce, RATE, 2, 0x7fffffff);
	return 0;
}

TIMER_OF_DECLARE(rp2350_timer, "raspberrypi,rp2350-timer", rp2350_timer_init);
