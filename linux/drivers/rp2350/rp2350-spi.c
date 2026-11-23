// SPDX-License-Identifier: GPL-2.0-only
/*
 * RP2350 SPI0 (an ARM PL022) as a Linux SPI controller for spidev: 8-bit
 * frames, modes 0-3, 100 kHz to 50 MHz set per transfer, the FIFO kept full
 * by polling (the controller makes the clock, so no device can hold a
 * transfer up). Chip select is a GPIO line (cs-gpios), held by the SPI core
 * for the whole message.
 *
 * Not upstream spi-pl022: that needs the AMBA bus and the common clock
 * framework, neither of which this kernel has.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/spi/spi.h>
#include "rp2350-pins.h"

#define SSPCR0		0x00
#define  CR0_DSS_8	7
#define  CR0_SPO	BIT(6)
#define  CR0_SPH	BIT(7)
#define SSPCR1		0x04
#define  CR1_SSE	BIT(1)
#define SSPDR		0x08
#define SSPSR		0x0c
#define  SR_TNF		BIT(1)
#define  SR_RNE		BIT(2)
#define  SR_BSY		BIT(4)
#define SSPCPSR		0x10
#define SSPIMSC		0x14
#define SSPDMACR	0x24
#define FIFO_DEPTH	8

#define MIN_HZ		100000
#define MAX_HZ		50000000

struct rp2350_spi {
	void __iomem *base;
	u32 clk_hz;
	u32 cur_hz, cur_cr0;
};

/* the pico-sdk's spi_set_baudrate: an even prescale, then a post divider */
static void rs_set_rate(struct rp2350_spi *s, u32 hz, u32 mode_bits)
{
	u32 pre, post, cr0;

	if (hz != s->cur_hz) {
		for (pre = 2; pre <= 254; pre += 2)
			if ((u64)s->clk_hz < (u64)(pre + 2) * 256 * hz)
				break;
		if (pre > 254)
			pre = 254;
		for (post = 256; post > 1; post--)
			if (s->clk_hz / (pre * (post - 1)) > hz)
				break;
		s->cur_hz = hz;
		writel(pre, s->base + SSPCPSR);
		s->cur_cr0 = (s->cur_cr0 & 0xff) | (post - 1) << 8;
	}
	cr0 = (s->cur_cr0 & ~0xff) | CR0_DSS_8 | mode_bits;
	if (cr0 != s->cur_cr0 || !(readl(s->base + SSPCR1) & CR1_SSE)) {
		writel(0, s->base + SSPCR1);
		writel(cr0, s->base + SSPCR0);
		writel(CR1_SSE, s->base + SSPCR1);
		s->cur_cr0 = cr0;
	}
}

static int rs_transfer_one(struct spi_controller *ctlr, struct spi_device *spi,
			   struct spi_transfer *t)
{
	struct rp2350_spi *s = spi_controller_get_devdata(ctlr);
	const u8 *tx = t->tx_buf;
	u8 *rx = t->rx_buf;
	unsigned int sent = 0, got = 0, len = t->len;
	u32 mode = (spi->mode & SPI_CPOL ? CR0_SPO : 0) | (spi->mode & SPI_CPHA ? CR0_SPH : 0);

	rs_set_rate(s, clamp_t(u32, t->speed_hz, MIN_HZ, MAX_HZ), mode);
	/* nothing left over in the receive FIFO */
	while (readl(s->base + SSPSR) & SR_RNE)
		readl(s->base + SSPDR);
	while (got < len) {
		u32 sr = readl(s->base + SSPSR);

		if (sent < len && sent - got < FIFO_DEPTH && (sr & SR_TNF)) {
			writel(tx ? tx[sent] : 0, s->base + SSPDR);
			sent++;
		}
		if (sr & SR_RNE) {
			u8 b = readl(s->base + SSPDR);

			if (rx)
				rx[got] = b;
			got++;
			if (!(got & 1023))
				cond_resched();
		}
	}
	return 0;
}

static int rp2350_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct rp2350_spi *s;
	u32 pins[3];
	int k;

	ctlr = devm_spi_alloc_host(dev, sizeof(*s));
	if (!ctlr)
		return -ENOMEM;
	s = spi_controller_get_devdata(ctlr);
	s->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(s->base))
		return PTR_ERR(s->base);
	/* rx, sck, tx */
	if (of_property_read_u32(dev->of_node, "clock-frequency", &s->clk_hz) ||
	    of_property_read_u32_array(dev->of_node, "raspberrypi,pins", pins, 3))
		return -EINVAL;
	rp2350_unreset(RP_RST_SPI0);
	writel(0, s->base + SSPCR1);
	writel(0, s->base + SSPIMSC);
	writel(0, s->base + SSPDMACR);
	for (k = 0; k < 3; k++) {
		if (pins[k] >= RP_NGPIO)
			return -EINVAL;
		rp2350_pad_digital(pins[k]);
		rp2350_pin_func(pins[k], RP_FUNC_SPI);
	}
	rs_set_rate(s, 1000000, 0);

	ctlr->dev.of_node = dev->of_node;
	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->use_gpio_descriptors = true;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->min_speed_hz = MIN_HZ;
	ctlr->max_speed_hz = MAX_HZ;
	ctlr->transfer_one = rs_transfer_one;
	return devm_spi_register_controller(dev, ctlr);
}

static const struct of_device_id rp2350_spi_of_match[] = {
	{ .compatible = "raspberrypi,rp2350-spi" },
	{ }
};

static struct platform_driver rp2350_spi_driver = {
	.probe = rp2350_spi_probe,
	.driver = {
		.name = "rp2350-spi",
		.of_match_table = rp2350_spi_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rp2350_spi_driver);

MODULE_LICENSE("GPL");
