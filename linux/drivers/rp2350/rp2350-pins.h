/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RP2350 pins on core 0: what the pin drivers (rp2350-gpio.c, -i2c.c,
 * -spi.c, -pwm.c, -adc.c, -pio.c) share. Core 1 (the boot stub, boot/)
 * owns USB, PIO0 with the Wi-Fi pins (GP23/24/25/29), SPI1 with the SD
 * card (GP8-GP11) and the PSRAM's GP0, and changes RESETS, IO_BANK0, PADS
 * and SIO for them too: everything here goes through the atomic SET/CLR
 * aliases (or SIO's own SET/CLR registers), never a read-modify-write of
 * a shared register.
 *
 * Addresses are physical, which on nommu are the virtual ones.
 */
#ifndef RP2350_PINS_H
#define RP2350_PINS_H

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/types.h>

#define RP_ALIAS_XOR		0x1000
#define RP_ALIAS_SET		0x2000
#define RP_ALIAS_CLR		0x3000

/* RESETS */
#define RP_RESETS		((void __iomem *)0x40020000)
#define RP_RESET		0x00
#define RP_RESET_DONE		0x08
#define RP_RST_ADC		BIT(0)
#define RP_RST_DMA		BIT(2)
#define RP_RST_I2C0		BIT(4)
#define RP_RST_I2C1		BIT(5)
#define RP_RST_PIO1		BIT(12)
#define RP_RST_PIO2		BIT(13)
#define RP_RST_PWM		BIT(16)
#define RP_RST_SPI0		BIT(18)

/* IO_BANK0: GPIOn_STATUS at 8n, GPIOn_CTRL at 8n + 4 */
#define RP_IO_BANK0		((void __iomem *)0x40028000)
#define RP_IO_CTRL(n)		(8 * (n) + 4)
#define RP_FUNC_SPI		1
#define RP_FUNC_I2C		3
#define RP_FUNC_PWM		4
#define RP_FUNC_SIO		5
#define RP_FUNC_PIO0		6	/* PIO1 7, PIO2 8 */
#define RP_FUNC_NULL		0x1f
#define RP_FUNC_MASK		0x1f

/* PADS_BANK0: GPIOn at 4 + 4n */
#define RP_PADS_BANK0		((void __iomem *)0x40038000)
#define RP_PAD(n)		(4 + 4 * (n))
#define RP_PAD_SCHMITT		BIT(1)
#define RP_PAD_PDE		BIT(2)
#define RP_PAD_PUE		BIT(3)
#define RP_PAD_IE		BIT(6)
#define RP_PAD_OD		BIT(7)
#define RP_PAD_ISO		BIT(8)

/* SIO GPIO registers: the HI ones hold GP32-GP47 in bits 0-15 */
#define RP_SIO			((void __iomem *)0xd0000000)
#define RP_SIO_IN		0x004
#define RP_SIO_OUT_SET		0x018
#define RP_SIO_OUT_CLR		0x020
#define RP_SIO_OE		0x030
#define RP_SIO_OE_SET		0x038
#define RP_SIO_OE_CLR		0x040
#define RP_SIO_HI		0x004	/* GPIO_HI_x = GPIO_x + 4 */

#define RP_NGPIO		48

static inline void rp2350_unreset(u32 bits)
{
	int i;

	writel(bits, RP_RESETS + RP_ALIAS_CLR + RP_RESET);
	for (i = 0; i < 1000 && (readl(RP_RESETS + RP_RESET_DONE) & bits) != bits; i++)
		udelay(1);
}

/* a pulse of reset, for a stuck controller */
static inline void rp2350_reset_pulse(u32 bits)
{
	writel(bits, RP_RESETS + RP_ALIAS_SET + RP_RESET);
	rp2350_unreset(bits);
}

/* GPIOn's function (no overrides); the pin must be ours */
static inline void rp2350_pin_func(unsigned int n, u32 func)
{
	writel(func, RP_IO_BANK0 + RP_IO_CTRL(n));
}

static inline u32 rp2350_pin_get_func(unsigned int n)
{
	return readl(RP_IO_BANK0 + RP_IO_CTRL(n)) & RP_FUNC_MASK;
}

static inline void rp2350_pad(unsigned int n, u32 set, u32 clr)
{
	if (clr)
		writel(clr, RP_PADS_BANK0 + RP_ALIAS_CLR + RP_PAD(n));
	if (set)
		writel(set, RP_PADS_BANK0 + RP_ALIAS_SET + RP_PAD(n));
}

/* a digital pin: input buffer on, not isolated, push-pull */
static inline void rp2350_pad_digital(unsigned int n)
{
	rp2350_pad(n, RP_PAD_IE, RP_PAD_ISO | RP_PAD_OD);
}

static inline void rp2350_sio_oe(unsigned int n, bool out)
{
	writel(BIT(n % 32), RP_SIO + (n >= 32 ? RP_SIO_HI : 0) + (out ? RP_SIO_OE_SET : RP_SIO_OE_CLR));
}

/* rp2350-gpio.c: the lines gpiolib may hand out (bit N = GPN) */
u64 rp2350_gpio_free_pins(void);

/* rp2350-pio.c: stop the PIO DMA channels around a flash erase or program
 * (XIP, where their buffers are, stops); register accesses only, safe with
 * interrupts off. pause returns what resume restarts. */
u32 rp2350_pio_dma_pause(void);
void rp2350_pio_dma_resume(u32 on);

#endif
