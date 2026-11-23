// SPDX-License-Identifier: GPL-2.0-only
/*
 * RP2350 bank 0 GPIO on core 0: GP0-GP47 as one gpiochip, with the line
 * names from the device tree and edge interrupts.
 *
 * A line gpiolib hands out goes to SIO (FUNCSEL 5) with its pad's input
 * buffer on and isolation off; levels and directions are SIO's SET/CLR
 * registers, pulls the pad's PUE/PDE (through the pad register's SET/CLR
 * aliases). Lines the board uses for something else are left out with the
 * device tree's gpio-reserved-ranges, so gpiolib never hands them out.
 *
 * Edge events: IO_BANK0 latches edges in INTR0-5 (4 bits a pin: level low,
 * level high, edge low, edge high); PROC0_INTE0-5 send them to core 0's
 * IO_IRQ_BANK0 (NVIC 21). Core 1 uses none of these.
 */
#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/platform_device.h>
#include "rp2350-pins.h"

#define IO_INTR(w)		(0x230 + 4 * (w))
#define IO_P0_INTE(w)		(0x248 + 4 * (w))
#define IO_P0_INTS(w)		(0x278 + 4 * (w))
#define IO_IRQ_WORDS		6
#define EV_EDGE_LOW		BIT(2)
#define EV_EDGE_HIGH		BIT(3)
#define EV_SHIFT(n)		(4 * ((n) % 8))

struct rp2350_gpio {
	struct gpio_chip gc;
	u8 edges[RP_NGPIO];	/* EV_EDGE_* wanted by each line's irq type */
};

static u64 rp_free_pins;

u64 rp2350_gpio_free_pins(void)
{
	return rp_free_pins;
}
EXPORT_SYMBOL_GPL(rp2350_gpio_free_pins);

static void __iomem *sio(unsigned int n, unsigned int reg)
{
	return RP_SIO + (n >= 32 ? RP_SIO_HI : 0) + reg;
}

static int rg_request(struct gpio_chip *gc, unsigned int n)
{
	/* an enabled PWM channel has the pin (rp2350-pwm.c): taking it here
	 * would leave the channel enabled with its pin on SIO */
	if (rp2350_pin_get_func(n) == RP_FUNC_PWM)
		return -EBUSY;
	rp2350_pad_digital(n);
	rp2350_pin_func(n, RP_FUNC_SIO);
	return 0;
}

static int rg_get_direction(struct gpio_chip *gc, unsigned int n)
{
	return readl(sio(n, RP_SIO_OE)) & BIT(n % 32) ? GPIO_LINE_DIRECTION_OUT :
							 GPIO_LINE_DIRECTION_IN;
}

static int rg_direction_input(struct gpio_chip *gc, unsigned int n)
{
	writel(BIT(n % 32), sio(n, RP_SIO_OE_CLR));
	return 0;
}

static void rg_set(struct gpio_chip *gc, unsigned int n, int val)
{
	writel(BIT(n % 32), sio(n, val ? RP_SIO_OUT_SET : RP_SIO_OUT_CLR));
}

static int rg_direction_output(struct gpio_chip *gc, unsigned int n, int val)
{
	rg_set(gc, n, val);
	writel(BIT(n % 32), sio(n, RP_SIO_OE_SET));
	return 0;
}

/* the pad's level (its input buffer is on, also for an output) */
static int rg_get(struct gpio_chip *gc, unsigned int n)
{
	return !!(readl(sio(n, RP_SIO_IN)) & BIT(n % 32));
}

static int rg_get_multiple(struct gpio_chip *gc, unsigned long *mask, unsigned long *bits)
{
	u32 lo = readl(RP_SIO + RP_SIO_IN), hi = readl(RP_SIO + RP_SIO_HI + RP_SIO_IN) & 0xffff;

	bits[0] = (bits[0] & ~mask[0]) | (lo & mask[0]);
	bits[1] = (bits[1] & ~mask[1]) | (hi & mask[1]);
	return 0;
}

static void rg_set_multiple(struct gpio_chip *gc, unsigned long *mask, unsigned long *bits)
{
	unsigned int w;

	for (w = 0; w < 2; w++) {
		unsigned long m = mask[w] & (w ? 0xffff : ~0UL);

		if (m & bits[w])
			writel(m & bits[w], RP_SIO + w * RP_SIO_HI + RP_SIO_OUT_SET);
		if (m & ~bits[w])
			writel(m & ~bits[w], RP_SIO + w * RP_SIO_HI + RP_SIO_OUT_CLR);
	}
}

static int rg_set_config(struct gpio_chip *gc, unsigned int n, unsigned long config)
{
	switch (pinconf_to_config_param(config)) {
	case PIN_CONFIG_BIAS_DISABLE:
		rp2350_pad(n, 0, RP_PAD_PUE | RP_PAD_PDE);
		return 0;
	case PIN_CONFIG_BIAS_PULL_UP:
		rp2350_pad(n, RP_PAD_PUE, RP_PAD_PDE);
		return 0;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		rp2350_pad(n, RP_PAD_PDE, RP_PAD_PUE);
		return 0;
	default:
		return -ENOTSUPP;
	}
}

static void rg_irq_ack(struct irq_data *d)
{
	irq_hw_number_t n = irqd_to_hwirq(d);

	writel((EV_EDGE_LOW | EV_EDGE_HIGH) << EV_SHIFT(n), RP_IO_BANK0 + IO_INTR(n / 8));
}

static void rg_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	irq_hw_number_t n = irqd_to_hwirq(d);

	writel((EV_EDGE_LOW | EV_EDGE_HIGH) << EV_SHIFT(n),
	       RP_IO_BANK0 + RP_ALIAS_CLR + IO_P0_INTE(n / 8));
	gpiochip_disable_irq(gc, n);
}

static void rg_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct rp2350_gpio *g = gpiochip_get_data(gc);
	irq_hw_number_t n = irqd_to_hwirq(d);

	gpiochip_enable_irq(gc, n);
	if (g->edges[n])
		writel(g->edges[n] << EV_SHIFT(n), RP_IO_BANK0 + RP_ALIAS_SET + IO_P0_INTE(n / 8));
}

static int rg_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct rp2350_gpio *g = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	irq_hw_number_t n = irqd_to_hwirq(d);
	u8 e = 0;

	if (type & ~IRQ_TYPE_EDGE_BOTH)
		return -EINVAL;	/* edges only */
	if (type & IRQ_TYPE_EDGE_RISING)
		e |= EV_EDGE_HIGH;
	if (type & IRQ_TYPE_EDGE_FALLING)
		e |= EV_EDGE_LOW;
	g->edges[n] = e;
	/* the enabled edges follow the new type (the core unmasks later) */
	writel(((EV_EDGE_LOW | EV_EDGE_HIGH) & ~e) << EV_SHIFT(n),
	       RP_IO_BANK0 + RP_ALIAS_CLR + IO_P0_INTE(n / 8));
	irq_set_handler_locked(d, handle_edge_irq);
	return 0;
}

static const struct irq_chip rg_irq_chip = {
	.name = "rp2350-gpio",
	.irq_ack = rg_irq_ack,
	.irq_mask = rg_irq_mask,
	.irq_unmask = rg_irq_unmask,
	.irq_set_type = rg_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE | IRQCHIP_SKIP_SET_WAKE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void rg_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long pend;
	unsigned int w, bit;

	chained_irq_enter(chip, desc);
	for (w = 0; w < IO_IRQ_WORDS; w++) {
		pend = readl(RP_IO_BANK0 + IO_P0_INTS(w)) & 0xcccccccc;
		while (pend) {
			bit = __ffs(pend);
			pend &= ~(0xfUL << (bit & ~3));
			generic_handle_domain_irq(gc->irq.domain, w * 8 + bit / 4);
		}
	}
	chained_irq_exit(chip, desc);
}

static int rp2350_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_irq_chip *girq;
	struct rp2350_gpio *g;
	unsigned int n, w;
	int irq, ret;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;

	g->gc.label = "rp2350-gpio";
	g->gc.parent = dev;
	g->gc.owner = THIS_MODULE;
	g->gc.base = -1;
	g->gc.ngpio = RP_NGPIO;
	g->gc.request = rg_request;
	g->gc.get_direction = rg_get_direction;
	g->gc.direction_input = rg_direction_input;
	g->gc.direction_output = rg_direction_output;
	g->gc.get = rg_get;
	g->gc.set = rg_set;
	g->gc.get_multiple = rg_get_multiple;
	g->gc.set_multiple = rg_set_multiple;
	g->gc.set_config = rg_set_config;

	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		/* no edge of ours enabled or latched from before */
		for (w = 0; w < IO_IRQ_WORDS; w++)
			writel(0, RP_IO_BANK0 + IO_P0_INTE(w));
		girq = &g->gc.irq;
		gpio_irq_chip_set_chip(girq, &rg_irq_chip);
		girq->parent_handler = rg_irq_handler;
		girq->num_parents = 1;
		girq->parents = devm_kcalloc(dev, 1, sizeof(*girq->parents), GFP_KERNEL);
		if (!girq->parents)
			return -ENOMEM;
		girq->parents[0] = irq;
		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_bad_irq;
	}
	ret = devm_gpiochip_add_data(dev, &g->gc, g);
	if (ret)
		return ret;
	for (n = 0; n < RP_NGPIO; n++)
		if (gpiochip_line_is_valid(&g->gc, n))
			rp_free_pins |= BIT_ULL(n);
	return 0;
}

static const struct of_device_id rp2350_gpio_of_match[] = {
	{ .compatible = "raspberrypi,rp2350-gpio" },
	{ }
};

static struct platform_driver rp2350_gpio_driver = {
	.probe = rp2350_gpio_probe,
	.driver = {
		.name = "rp2350-gpio",
		.of_match_table = rp2350_gpio_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rp2350_gpio_driver);

MODULE_LICENSE("GPL");
