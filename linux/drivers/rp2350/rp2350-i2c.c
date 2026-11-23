// SPDX-License-Identifier: GPL-2.0-only
/*
 * RP2350 I2C0/I2C1 (Synopsys DesignWare blocks) as Linux I2C adapters,
 * driven at 100 kHz with the pads' pull-ups, one byte in flight as the
 * pico-sdk does, and every wait bounded (5 ms a byte). Past that the
 * controller is reset, a device still holding SDA low gets up to 9 SCL
 * pulses and a STOP, and the transfer fails with -ETIMEDOUT. The controller can't send an address without data, so a
 * zero-length write (SMBus quick, i2cdetect's probe) becomes a one-byte
 * read.
 *
 * Not upstream i2c-designware: that needs the common clock framework for
 * its timings and refuses zero-length messages, so i2cdetect would skip
 * most addresses.
 */
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "rp2350-pins.h"

#define IC_CON			0x00
#define  CON_MASTER		BIT(0)
#define  CON_SPEED_FAST		(2 << 1)
#define  CON_RESTART_EN		BIT(5)
#define  CON_SLAVE_DISABLE	BIT(6)
#define  CON_TX_EMPTY_CTRL	BIT(8)
#define IC_TAR			0x04
#define IC_DATA_CMD		0x10
#define  CMD_READ		BIT(8)
#define  CMD_STOP		BIT(9)
#define  CMD_RESTART		BIT(10)
#define IC_FS_SCL_HCNT		0x1c
#define IC_FS_SCL_LCNT		0x20
#define IC_INTR_MASK		0x30
#define IC_RAW_INTR_STAT	0x34
#define  INTR_TX_EMPTY		BIT(4)
#define  INTR_STOP_DET		BIT(9)
#define IC_RX_TL		0x38
#define IC_TX_TL		0x3c
#define IC_CLR_TX_ABRT		0x54
#define IC_CLR_STOP_DET		0x60
#define IC_ENABLE		0x6c
#define IC_RXFLR		0x78
#define IC_SDA_HOLD		0x7c
#define IC_TX_ABRT_SOURCE	0x80
#define  ABRT_7B_ADDR_NOACK	BIT(0)
#define IC_ENABLE_STATUS	0x9c
#define IC_FS_SPKLEN		0xa0

#define BYTE_US			5000
#define BUS_HZ			100000

struct rp2350_i2c {
	struct i2c_adapter adap;
	void __iomem *base;
	u32 reset_bit;
	unsigned int sda, scl;
	u32 clk_hz;
};

static u32 rd(struct rp2350_i2c *i, u32 reg)
{
	return readl(i->base + reg);
}

static void wr(struct rp2350_i2c *i, u32 reg, u32 v)
{
	writel(v, i->base + reg);
}

/* until (rd(reg) & mask) is (want ? mask : 0) or us have passed */
static bool spin(struct rp2350_i2c *i, u32 reg, u32 mask, bool want, unsigned int us)
{
	ktime_t end = ktime_add_us(ktime_get(), us);

	for (;;) {
		bool got = (rd(i, reg) & mask) == (want ? mask : 0);

		if (got)
			return true;
		if (ktime_after(ktime_get(), end))
			return (rd(i, reg) & mask) == (want ? mask : 0);
		cpu_relax();
	}
}

/* the pico-sdk's i2c_init(BUS_HZ), controller disabled */
static void ri_setup(struct rp2350_i2c *i)
{
	u32 period = (i->clk_hz + BUS_HZ / 2) / BUS_HZ;
	u32 lcnt = period * 3 / 5, hcnt = period - lcnt;

	wr(i, IC_ENABLE, 0);
	wr(i, IC_CON, CON_SPEED_FAST | CON_MASTER | CON_SLAVE_DISABLE | CON_RESTART_EN |
		      CON_TX_EMPTY_CTRL);
	wr(i, IC_TX_TL, 0);
	wr(i, IC_RX_TL, 0);
	wr(i, IC_INTR_MASK, 0);
	wr(i, IC_FS_SCL_HCNT, hcnt);
	wr(i, IC_FS_SCL_LCNT, lcnt);
	wr(i, IC_FS_SPKLEN, lcnt < 16 ? 1 : lcnt / 16);
	wr(i, IC_SDA_HOLD, (rd(i, IC_SDA_HOLD) & ~0xffff) | ((i->clk_hz * 3) / 10000000 + 1));
}

static int ri_abort_error(struct rp2350_i2c *i)
{
	u32 src = rd(i, IC_TX_ABRT_SOURCE);

	rd(i, IC_CLR_TX_ABRT);
	return src & ABRT_7B_ADDR_NOACK ? -ENXIO : -EIO;
}

/* after an abort the controller ends the transfer with a STOP */
static int ri_aborted(struct rp2350_i2c *i)
{
	int e = ri_abort_error(i);

	if (spin(i, IC_RAW_INTR_STAT, INTR_STOP_DET, true, BYTE_US))
		rd(i, IC_CLR_STOP_DET);
	return e;
}

static int sda_level(struct rp2350_i2c *i)
{
	return !!(readl(RP_SIO + RP_SIO_IN) & BIT(i->sda));
}

/*
 * The bus stayed busy past the deadline: reset the controller (that lets
 * go of the lines), clock a device holding SDA free (9 SCL pulses, then a
 * STOP, with the pins on SIO as open drain), and set it up again.
 */
static int ri_timed_out(struct rp2350_i2c *i)
{
	unsigned int k;

	rp2350_reset_pulse(i->reset_bit);
	if (!sda_level(i)) {
		rp2350_sio_oe(i->sda, false);
		rp2350_sio_oe(i->scl, false);
		writel(BIT(i->sda) | BIT(i->scl), RP_SIO + RP_SIO_OUT_CLR);
		rp2350_pin_func(i->sda, RP_FUNC_SIO);
		rp2350_pin_func(i->scl, RP_FUNC_SIO);
		for (k = 0; k < 9 && !sda_level(i); k++) {
			rp2350_sio_oe(i->scl, true);
			udelay(5);
			rp2350_sio_oe(i->scl, false);
			udelay(5);
		}
		rp2350_sio_oe(i->sda, true);
		udelay(5);
		rp2350_sio_oe(i->scl, false);
		udelay(5);
		rp2350_sio_oe(i->sda, false);
		udelay(5);
		rp2350_pin_func(i->sda, RP_FUNC_I2C);
		rp2350_pin_func(i->scl, RP_FUNC_I2C);
	}
	ri_setup(i);
	return -ETIMEDOUT;
}

static int ri_xfer_one(struct rp2350_i2c *i, struct i2c_msg *msgs, int num)
{
	int k, j;

	wr(i, IC_ENABLE, 0);
	if (!spin(i, IC_ENABLE_STATUS, 1, false, BYTE_US))
		return ri_timed_out(i);
	wr(i, IC_TAR, msgs[0].addr);
	rd(i, IC_CLR_TX_ABRT);
	rd(i, IC_CLR_STOP_DET);
	wr(i, IC_ENABLE, 1);
	for (k = 0; k < num; k++) {
		struct i2c_msg *m = &msgs[k];
		bool read = m->flags & I2C_M_RD;

		for (j = 0; j < m->len; j++) {
			bool stop = k + 1 == num && j + 1 == m->len;
			/* a repeated start before each message after the first */
			u32 cmd = (stop ? CMD_STOP : 0) | (k && !j ? CMD_RESTART : 0);

			if (!read) {
				wr(i, IC_DATA_CMD, cmd | m->buf[j]);
				/* TX_EMPTY_CTRL: empty once the byte has left */
				if (!spin(i, IC_RAW_INTR_STAT, INTR_TX_EMPTY, true, BYTE_US))
					return ri_timed_out(i);
				if (rd(i, IC_TX_ABRT_SOURCE))
					return ri_aborted(i);
			} else {
				ktime_t end = ktime_add_us(ktime_get(), BYTE_US);
				bool ok;

				wr(i, IC_DATA_CMD, cmd | CMD_READ);
				while (!(ok = rd(i, IC_RXFLR) || rd(i, IC_TX_ABRT_SOURCE)) &&
				       !ktime_after(ktime_get(), end))
					cpu_relax();
				if (rd(i, IC_TX_ABRT_SOURCE))
					return ri_aborted(i);
				if (!ok && !rd(i, IC_RXFLR))
					return ri_timed_out(i);
				m->buf[j] = rd(i, IC_DATA_CMD) & 0xff;
			}
			if (stop) {
				if (!spin(i, IC_RAW_INTR_STAT, INTR_STOP_DET, true, BYTE_US))
					return ri_timed_out(i);
				rd(i, IC_CLR_STOP_DET);
				/* an abort can show only once the STOP is out */
				if (rd(i, IC_TX_ABRT_SOURCE))
					return ri_abort_error(i);
			}
		}
	}
	return num;
}

static int ri_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct rp2350_i2c *i = i2c_get_adapdata(adap);
	u8 dummy;
	int k, ret;

	for (k = 0; k < num; k++) {
		if (msgs[k].addr != msgs[0].addr || msgs[k].flags & (I2C_M_TEN | I2C_M_NOSTART))
			return -EOPNOTSUPP;
		if (!msgs[k].len && num > 1)
			return -EINVAL;
	}
	/* an address alone: a one-byte read asks the same question */
	if (num == 1 && !msgs[0].len) {
		struct i2c_msg m = { .addr = msgs[0].addr, .flags = I2C_M_RD, .len = 1, .buf = &dummy };

		ret = ri_xfer_one(i, &m, 1);
		return ret < 0 ? ret : 1;
	}
	return ri_xfer_one(i, msgs, num);
}

static u32 ri_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm ri_algo = {
	.master_xfer = ri_xfer,
	.functionality = ri_func,
};

static int rp2350_i2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rp2350_i2c *i;
	u32 pins[2];

	i = devm_kzalloc(dev, sizeof(*i), GFP_KERNEL);
	if (!i)
		return -ENOMEM;
	i->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i->base))
		return PTR_ERR(i->base);
	if (of_property_read_u32(dev->of_node, "clock-frequency", &i->clk_hz) ||
	    of_property_read_u32_array(dev->of_node, "raspberrypi,pins", pins, 2) ||
	    pins[0] >= 32 || pins[1] >= 32)
		return -EINVAL;
	i->sda = pins[0];
	i->scl = pins[1];
	i->reset_bit = (pdev->resource[0].start & 0x8000) ? RP_RST_I2C1 : RP_RST_I2C0;
	rp2350_unreset(i->reset_bit);
	ri_setup(i);
	/* the pins: I2C with the pads' pull-ups */
	rp2350_pad(i->sda, RP_PAD_IE | RP_PAD_PUE | RP_PAD_SCHMITT, RP_PAD_ISO | RP_PAD_OD | RP_PAD_PDE);
	rp2350_pad(i->scl, RP_PAD_IE | RP_PAD_PUE | RP_PAD_SCHMITT, RP_PAD_ISO | RP_PAD_OD | RP_PAD_PDE);
	rp2350_sio_oe(i->sda, false);
	rp2350_sio_oe(i->scl, false);
	rp2350_pin_func(i->sda, RP_FUNC_I2C);
	rp2350_pin_func(i->scl, RP_FUNC_I2C);

	i->adap.owner = THIS_MODULE;
	i->adap.algo = &ri_algo;
	i->adap.dev.parent = dev;
	i->adap.dev.of_node = dev->of_node;
	i->adap.nr = -1;
	snprintf(i->adap.name, sizeof(i->adap.name), "rp2350-i2c at GP%u/GP%u", i->sda, i->scl);
	i2c_set_adapdata(&i->adap, i);
	platform_set_drvdata(pdev, i);
	return devm_i2c_add_adapter(dev, &i->adap);
}

static const struct of_device_id rp2350_i2c_of_match[] = {
	{ .compatible = "raspberrypi,rp2350-i2c" },
	{ }
};

static struct platform_driver rp2350_i2c_driver = {
	.probe = rp2350_i2c_probe,
	.driver = {
		.name = "rp2350-i2c",
		.of_match_table = rp2350_i2c_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rp2350_i2c_driver);

MODULE_LICENSE("GPL");
