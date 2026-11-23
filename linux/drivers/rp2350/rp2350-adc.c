// SPDX-License-Identifier: GPL-2.0-only
/*
 * RP2350B ADC as an IIO device: in_voltage0..7 are GP40-GP47, in_temp the
 * chip's temperature sensor (ADC input 8). One-shot conversions (2 us at
 * clk_adc's 48 MHz), polled.
 */
#include <linux/iio/iio.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "rp2350-pins.h"

#define ADC_CS		0x00
#define  CS_EN		BIT(0)
#define  CS_TS_EN	BIT(1)
#define  CS_START_ONCE	BIT(2)
#define  CS_READY	BIT(8)
#define  CS_ERR		BIT(9)
#define  CS_AINSEL(n)	((n) << 12)
#define ADC_RESULT	0x04
#define ADC_FCS		0x08
#define ADC_DIV		0x10
#define ADC_INTE	0x18
#define TEMP_CH		8
#define VREF_MV		3300
#define ADC_PIN0	40

struct rp2350_adc {
	void __iomem *base;
	struct mutex lock;
};

#define RA_VOLTAGE(ch) {						\
	.type = IIO_VOLTAGE,						\
	.indexed = 1,							\
	.channel = (ch),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
}

static const struct iio_chan_spec ra_channels[] = {
	RA_VOLTAGE(0), RA_VOLTAGE(1), RA_VOLTAGE(2), RA_VOLTAGE(3),
	RA_VOLTAGE(4), RA_VOLTAGE(5), RA_VOLTAGE(6), RA_VOLTAGE(7),
	{
		.type = IIO_TEMP,
		.channel = TEMP_CH,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_OFFSET) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int ra_convert(struct rp2350_adc *a, unsigned int ch, int *val)
{
	u32 cs;
	int ret;

	mutex_lock(&a->lock);
	writel(CS_EN | CS_TS_EN | CS_AINSEL(ch) | CS_START_ONCE, a->base + ADC_CS);
	ret = readl_poll_timeout(a->base + ADC_CS, cs, cs & CS_READY, 0, 1000);
	if (!ret) {
		*val = readl(a->base + ADC_RESULT) & 0xfff;
		if (readl(a->base + ADC_CS) & CS_ERR)
			ret = -EIO;
	}
	mutex_unlock(&a->lock);
	return ret;
}

/*
 * RP2350 datasheet, temperature sensor: T = 27 - (V - 0.706) / 0.001721 (degC).
 * With V = raw * 3.3 / 4096 that is (raw - 933.977) * -468.135 m°C.
 */
static int ra_read_raw(struct iio_dev *iio, struct iio_chan_spec const *ch,
		       int *val, int *val2, long mask)
{
	struct rp2350_adc *a = iio_priv(iio);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = ra_convert(a, ch->channel, val);
		return ret ? ret : IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		if (ch->type == IIO_TEMP) {
			*val = -468;
			*val2 = 135000;
			return IIO_VAL_INT_PLUS_MICRO;
		}
		*val = VREF_MV;
		*val2 = 12;
		return IIO_VAL_FRACTIONAL_LOG2;
	case IIO_CHAN_INFO_OFFSET:
		*val = -933;
		*val2 = 977000;
		return IIO_VAL_INT_PLUS_MICRO;
	}
	return -EINVAL;
}

static const struct iio_info ra_info = {
	.read_raw = ra_read_raw,
};

static int rp2350_adc_probe(struct platform_device *pdev)
{
	struct iio_dev *iio;
	struct rp2350_adc *a;
	unsigned int n;
	u32 cs;

	iio = devm_iio_device_alloc(&pdev->dev, sizeof(*a));
	if (!iio)
		return -ENOMEM;
	a = iio_priv(iio);
	mutex_init(&a->lock);
	a->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(a->base))
		return PTR_ERR(a->base);
	rp2350_unreset(RP_RST_ADC);
	writel(0, a->base + ADC_FCS);
	writel(0, a->base + ADC_INTE);
	writel(0, a->base + ADC_DIV);
	writel(CS_EN | CS_TS_EN, a->base + ADC_CS);
	if (readl_poll_timeout(a->base + ADC_CS, cs, cs & CS_READY, 0, 10000))
		dev_warn(&pdev->dev, "ADC not ready (clk_adc off?)\n");
	/* the ADC pins: no digital function, input buffer off, no pulls */
	for (n = ADC_PIN0; n < ADC_PIN0 + 8; n++) {
		rp2350_pin_func(n, RP_FUNC_NULL);
		rp2350_pad(n, RP_PAD_OD, RP_PAD_IE | RP_PAD_PUE | RP_PAD_PDE | RP_PAD_ISO);
	}
	iio->name = "rp2350-adc";
	iio->info = &ra_info;
	iio->modes = INDIO_DIRECT_MODE;
	iio->channels = ra_channels;
	iio->num_channels = ARRAY_SIZE(ra_channels);
	return devm_iio_device_register(&pdev->dev, iio);
}

static const struct of_device_id rp2350_adc_of_match[] = {
	{ .compatible = "raspberrypi,rp2350-adc" },
	{ }
};

static struct platform_driver rp2350_adc_driver = {
	.probe = rp2350_adc_probe,
	.driver = {
		.name = "rp2350-adc",
		.of_match_table = rp2350_adc_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rp2350_adc_driver);

MODULE_LICENSE("GPL");
