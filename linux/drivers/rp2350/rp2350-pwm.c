// SPDX-License-Identifier: GPL-2.0-only
/*
 * RP2350 PWM as one pwmchip with a channel per GPIO (channel N drives GPN,
 * N < 40): the slice and channel (A/B) that reach the pin, the divider
 * and TOP worked out from the period (the longest period and duty at or
 * below the ones asked for). Pins on one slice share its counter: a
 * slice's period can't change while another of its pins is running, and
 * two pins on the same slice channel (GP0 and GP16, say) share its duty.
 *
 * Enabling a channel gives the pin to the PWM (FUNCSEL 4); disabling it
 * makes the pin a SIO input again. Pins gpiolib doesn't hand out (the
 * board's, the ADC's) and pins a PIO block has are refused.
 */
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/spinlock.h>
#include "rp2350-pins.h"

#define NPWM		40
#define NSLICE		12
#define CH_CSR(s)	(0x14 * (s) + 0x00)
#define  CSR_EN		BIT(0)
#define  CSR_A_INV	BIT(2)
#define  CSR_B_INV	BIT(3)
#define CH_DIV(s)	(0x14 * (s) + 0x04)
#define CH_CTR(s)	(0x14 * (s) + 0x08)
#define CH_CC(s)	(0x14 * (s) + 0x0c)
#define CH_TOP(s)	(0x14 * (s) + 0x10)

struct rp_chan {
	bool on, inv;
	u32 period_ns, duty_ns;	/* what the hardware does */
};

struct rp2350_pwm {
	void __iomem *base;
	spinlock_t lock;
	u32 clk_hz;
	struct rp_chan ch[NPWM];
	u32 slice_div16[NSLICE], slice_top[NSLICE];
};

static void pwm_slice(unsigned int n, unsigned int *s, unsigned int *c)
{
	*s = n < 32 ? (n >> 1) & 7 : 8 + ((n >> 1) & 3);
	*c = n & 1;
}

/* the running pins on n's slice other than n */
static u64 slice_users(struct rp2350_pwm *p, unsigned int n)
{
	unsigned int s, c, k, ks, kc;
	u64 m = 0;

	pwm_slice(n, &s, &c);
	for (k = 0; k < NPWM; k++) {
		pwm_slice(k, &ks, &kc);
		if (k != n && ks == s && p->ch[k].on)
			m |= BIT_ULL(k);
	}
	return m;
}

/* The divider (in 16ths, 1 to 255 15/16) and TOP for period_ns: the
 * smallest divider whose count fits in 16 bits. *rperiod and *rduty are the
 * period and duty the hardware then runs. */
static int pwm_timing(u32 clk, u64 period_ns, u64 duty_ns, u32 *div16, u32 *top, u32 *cc,
		      u32 *rperiod, u32 *rduty)
{
	const u64 NS = NSEC_PER_SEC;
	u64 t16, d, wrap, c;

	period_ns = min(period_ns, NS);
	t16 = div64_u64(period_ns * clk * 16, NS);
	d = clamp_t(u64, DIV_ROUND_UP_ULL(t16, 1 << 16), 16, 0xfff);
	wrap = min_t(u64, div64_u64(t16, d), 1 << 16);
	if (wrap < 2)
		return -EINVAL;
	duty_ns = min(duty_ns, period_ns);
	c = min_t(u64, min_t(u64, div64_u64(div64_u64(duty_ns * clk * 16, NS), d), wrap), 0xffff);
	*div16 = d;
	*top = wrap - 1;
	*cc = c;
	*rperiod = div64_u64(wrap * d * NS / 16, clk);
	*rduty = div64_u64(c * d * NS / 16, clk);
	return 0;
}

static int rp_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	unsigned int n = pwm->hwpwm;
	u32 f;

	if (!(rp2350_gpio_free_pins() & BIT_ULL(n)))
		return -EBUSY;
	f = rp2350_pin_get_func(n);
	if (f >= RP_FUNC_PIO0 && f <= RP_FUNC_PIO0 + 2)
		return -EBUSY;
	return 0;
}

static int rp_apply(struct pwm_chip *chip, struct pwm_device *pwm, const struct pwm_state *st)
{
	struct rp2350_pwm *p = pwmchip_get_drvdata(chip);
	unsigned int n = pwm->hwpwm, s, c, k, ks, kc;
	struct rp_chan *ch = &p->ch[n];
	u32 div16, top, cc, per, duty, csr, inv;
	unsigned long flags;
	bool same_ch;
	u64 users;
	int ret = 0;

	pwm_slice(n, &s, &c);
	inv = c ? CSR_B_INV : CSR_A_INV;
	spin_lock_irqsave(&p->lock, flags);
	users = slice_users(p, n);
	if (!st->enabled) {
		if (ch->on) {
			same_ch = false;
			for (k = 0; k < NPWM; k++) {
				pwm_slice(k, &ks, &kc);
				if (users & BIT_ULL(k) && kc == c)
					same_ch = true;
			}
			/* another pin on the same slice channel keeps it running */
			if (!same_ch) {
				if (c)
					writel(readl(p->base + CH_CC(s)) & 0xffff, p->base + CH_CC(s));
				else
					writel(readl(p->base + CH_CC(s)) & 0xffff0000, p->base + CH_CC(s));
				writel(readl(p->base + CH_CSR(s)) & ~inv, p->base + CH_CSR(s));
			}
			if (!users)
				writel(0, p->base + CH_CSR(s));
			ch->on = false;
			rp2350_sio_oe(n, false);
			rp2350_pin_func(n, RP_FUNC_SIO);
		}
		ch->inv = st->polarity == PWM_POLARITY_INVERSED;
		goto out;
	}
	if (pwm_timing(p->clk_hz, st->period, st->duty_cycle, &div16, &top, &cc, &per, &duty)) {
		ret = -EINVAL;
		goto out;
	}
	/* pins sharing the slice share its counter: the period can't change */
	if (users && (p->slice_div16[s] != div16 || p->slice_top[s] != top)) {
		ret = -EINVAL;
		goto out;
	}
	csr = readl(p->base + CH_CSR(s));
	if (!users) {
		writel(0, p->base + CH_CSR(s));
		writel(div16, p->base + CH_DIV(s));
		writel(top, p->base + CH_TOP(s));
		writel(0, p->base + CH_CTR(s));
		csr = 0;
	}
	if (c)
		writel((readl(p->base + CH_CC(s)) & 0xffff) | cc << 16, p->base + CH_CC(s));
	else
		writel((readl(p->base + CH_CC(s)) & 0xffff0000) | cc, p->base + CH_CC(s));
	csr = (csr & ~inv) | (st->polarity == PWM_POLARITY_INVERSED ? inv : 0) | CSR_EN;
	writel(csr, p->base + CH_CSR(s));
	p->slice_div16[s] = div16;
	p->slice_top[s] = top;
	if (!ch->on) {
		rp2350_pad_digital(n);
		rp2350_pin_func(n, RP_FUNC_PWM);
	}
	ch->on = true;
	ch->inv = st->polarity == PWM_POLARITY_INVERSED;
	ch->period_ns = per;
	ch->duty_ns = duty;
out:
	spin_unlock_irqrestore(&p->lock, flags);
	return ret;
}

static int rp_get_state(struct pwm_chip *chip, struct pwm_device *pwm, struct pwm_state *st)
{
	struct rp2350_pwm *p = pwmchip_get_drvdata(chip);
	struct rp_chan *ch = &p->ch[pwm->hwpwm];

	st->enabled = ch->on;
	st->period = ch->period_ns;
	st->duty_cycle = ch->duty_ns;
	st->polarity = ch->inv ? PWM_POLARITY_INVERSED : PWM_POLARITY_NORMAL;
	return 0;
}

static const struct pwm_ops rp_ops = {
	.request = rp_request,
	.apply = rp_apply,
	.get_state = rp_get_state,
};

static int rp2350_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct rp2350_pwm *p;
	void __iomem *base;
	unsigned int s;
	u32 clk;

	if (of_property_read_u32(dev->of_node, "clock-frequency", &clk))
		return -EINVAL;
	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	chip = devm_pwmchip_alloc(dev, NPWM, sizeof(*p));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	p = pwmchip_get_drvdata(chip);
	p->base = base;
	p->clk_hz = clk;
	spin_lock_init(&p->lock);
	rp2350_unreset(RP_RST_PWM);
	for (s = 0; s < NSLICE; s++)
		writel(0, base + CH_CSR(s));
	chip->ops = &rp_ops;
	chip->atomic = true;
	return devm_pwmchip_add(dev, chip);
}

static const struct of_device_id rp2350_pwm_of_match[] = {
	{ .compatible = "raspberrypi,rp2350-pwm" },
	{ }
};

static struct platform_driver rp2350_pwm_driver = {
	.probe = rp2350_pwm_probe,
	.driver = {
		.name = "rp2350-pwm",
		.of_match_table = rp2350_pwm_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rp2350_pwm_driver);

MODULE_LICENSE("GPL");
