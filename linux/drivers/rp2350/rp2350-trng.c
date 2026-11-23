// SPDX-License-Identifier: GPL-2.0
/*
 * RP2350 TRNG (0x400f0000) as a hwrng: the hwrng core's thread feeds it to
 * the kernel's random pool with full credit, so the CRNG is ready within a
 * second of boot. Without it the kernel gathers jitter entropy on its own,
 * 20-70 s on this chip, and getrandom() callers (dropbear) wait for that.
 *
 * The block is Arm's CryptoCell-312 TRNG: a ring oscillator sampled every
 * SAMPLE_CNT1 clocks, through a von Neumann balancer and the CRNGT and
 * autocorrelation tests, into a 192-bit EHR. RNG_ISR.EHR_VALID says the
 * EHR is full; reading it and clearing the bit starts the next collection.
 * A test failure throws the collection away (the ISR error bits); four
 * autocorrelation failures in a row stop the block until a software reset.
 */
#include <linux/delay.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "rp2350-pins.h"

#define RNG_IMR			0x100
#define RNG_ISR			0x104
#define RNG_ICR			0x108
#define TRNG_CONFIG		0x10c
#define EHR_DATA0		0x114	/* ...EHR_DATA5 at 0x128 */
#define RND_SOURCE_ENABLE	0x12c
#define SAMPLE_CNT1		0x130
#define TRNG_DEBUG_CONTROL	0x138
#define TRNG_SW_RESET		0x140
#define RST_BITS_COUNTER	0x1bc

#define ISR_EHR_VALID		BIT(0)
#define ISR_AUTOCORR_ERR	BIT(1)
#define ISR_CRNGT_ERR		BIT(2)
#define ISR_VN_ERR		BIT(3)
#define ISR_ALL			0xf

#define RP_RST_TRNG		BIT(25)

/* clocks between samples of the oscillator; at 348 MHz 1000 is ~3 us, so
 * one 192-bit EHR (the balancer keeps about 1 sample in 4) takes ~2 ms */
#define RT_SAMPLE_CNT		1000
#define RT_EHR_BYTES		24

struct rp2350_trng {
	struct hwrng rng;
	void __iomem *base;
	unsigned long restarts, errors;
};

static void rt_start(struct rp2350_trng *t)
{
	int i;

	writel(0, t->base + RND_SOURCE_ENABLE);
	writel(1, t->base + TRNG_SW_RESET);
	/* the reset takes a few clocks; SAMPLE_CNT1 reads back once it's done */
	for (i = 0; i < 1000; i++) {
		writel(RT_SAMPLE_CNT, t->base + SAMPLE_CNT1);
		if (readl(t->base + SAMPLE_CNT1) == RT_SAMPLE_CNT)
			break;
		udelay(1);
	}
	writel(ISR_ALL, t->base + RNG_IMR);	/* no interrupts: we poll */
	writel(0, t->base + TRNG_CONFIG);	/* shortest inverter chain */
	writel(0, t->base + TRNG_DEBUG_CONTROL);	/* every test on */
	writel(ISR_ALL, t->base + RNG_ICR);
	writel(1, t->base + RST_BITS_COUNTER);
	writel(1, t->base + RND_SOURCE_ENABLE);
	t->restarts++;
}

static int rt_read(struct hwrng *rng, void *buf, size_t max, bool wait)
{
	struct rp2350_trng *t = container_of(rng, struct rp2350_trng, rng);
	unsigned long deadline = jiffies + msecs_to_jiffies(100);
	u32 ehr[RT_EHR_BYTES / 4], isr;
	unsigned int i;

	for (;;) {
		isr = readl(t->base + RNG_ISR);
		if (isr & ISR_AUTOCORR_ERR) {		/* stopped: start over */
			t->errors++;
			rt_start(t);
		} else if (isr & (ISR_CRNGT_ERR | ISR_VN_ERR)) {	/* that collection is lost */
			t->errors++;
			writel(isr & (ISR_CRNGT_ERR | ISR_VN_ERR), t->base + RNG_ICR);
		} else if (isr & ISR_EHR_VALID) {
			break;
		}
		if (!wait)
			return 0;
		if (time_after(jiffies, deadline)) {	/* stuck: start over once */
			rt_start(t);
			return 0;
		}
		usleep_range(500, 1000);
	}
	for (i = 0; i < ARRAY_SIZE(ehr); i++)
		ehr[i] = readl(t->base + EHR_DATA0 + 4 * i);
	writel(ISR_EHR_VALID, t->base + RNG_ICR);	/* next collection */
	max = min_t(size_t, max, sizeof(ehr));
	memcpy(buf, ehr, max);
	memzero_explicit(ehr, sizeof(ehr));
	return max;
}

static int rp2350_trng_probe(struct platform_device *pdev)
{
	struct rp2350_trng *t;
	int err;

	t = devm_kzalloc(&pdev->dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(t->base))
		return PTR_ERR(t->base);
	rp2350_unreset(RP_RST_TRNG);
	rt_start(t);
	t->rng.name = "rp2350-trng";
	t->rng.read = rt_read;
	t->rng.quality = 1024;		/* full entropy per bit, after the tests */
	err = devm_hwrng_register(&pdev->dev, &t->rng);
	if (err)
		return dev_err_probe(&pdev->dev, err, "hwrng_register\n");
	dev_info(&pdev->dev, "TRNG registered\n");
	return 0;
}

static const struct of_device_id rp2350_trng_of_match[] = {
	{ .compatible = "raspberrypi,rp2350-trng" },
	{ }
};

static struct platform_driver rp2350_trng_driver = {
	.probe = rp2350_trng_probe,
	.driver = {
		.name = "rp2350-trng",
		.of_match_table = rp2350_trng_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rp2350_trng_driver);

MODULE_LICENSE("GPL");
