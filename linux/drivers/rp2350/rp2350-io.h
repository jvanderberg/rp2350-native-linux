/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RP2350: what Linux on core 0 shares with the I/O firmware on core 1
 * (boot/src/io.rs: keep the two in step).
 *
 * The I/O area, 16 KiB of SRAM at 0x20068000 (the device tree's nodes give
 * it as their reg). Little-endian u32s; each side writes only its own
 * fields; ring indices run free (index % slots is the slot).
 *
 *   0x000 magic "RPIO", version
 *   0x040 network control         0x080 SD card control (segments at 0x0c0)
 *   0x100 flash control           0x200 flash page buffer (256 bytes)
 *   0x400 flash routine's stack   0x800 network receive ring
 *   0x2000 network transmit ring
 *
 * Doorbells: core 1 rings core 0's (SIO_IRQ_BELL, NVIC IRQ 26, shared by
 * the drivers, each with its bit); Linux rings core 1's to wake it.
 */
#ifndef RP2350_IO_H
#define RP2350_IO_H

#include <linux/io.h>

#define RPIO_MAGIC		0x4f495052	/* "RPIO" */
#define RPIO_SIZE		0x4000

/* network control (0x040) */
#define RPIO_NET_RADIO		0x040	/* 0 starting, 1 up, 2 absent */
#define RPIO_NET_LINK		0x044
#define RPIO_NET_MAC_LO		0x048
#define RPIO_NET_MAC_HI		0x04c
#define RPIO_NET_RX_HEAD	0x050	/* Linux */
#define RPIO_NET_RX_TAIL	0x054	/* core 1 */
#define RPIO_NET_TX_HEAD	0x058	/* core 1 */
#define RPIO_NET_TX_TAIL	0x05c	/* Linux */
#define RPIO_NET_TX_WAKE	0x060	/* Linux sets, core 1 clears and rings */
#define RPIO_NET_REJOINS	0x064
#define RPIO_NET_JOINS		0x068
#define RPIO_NET_JOIN_FAILS	0x06c
#define RPIO_NET_CMD		0x070	/* Linux sets, core 1 clears */
#define RPIO_NET_CMD_REJOIN	1	/* leave and join again (a test) */
#define RPIO_NET_SLOTS		4
#define RPIO_NET_SLOT		0x600	/* u32 length, then the frame */
#define RPIO_NET_FRAME_MAX	(RPIO_NET_SLOT - 4)
#define RPIO_NET_RX_RING	0x800
#define RPIO_NET_TX_RING	0x2000
#define RPIO_RADIO_STARTING	0
#define RPIO_RADIO_UP		1
#define RPIO_RADIO_ABSENT	2

/* SD card control (0x080) */
#define RPIO_BLK_SECTORS_LO	0x080	/* core 1: 0 while no card */
#define RPIO_BLK_SECTORS_HI	0x084
#define RPIO_BLK_GEN		0x088	/* core 1: bumped on a change of card */
#define RPIO_BLK_REQ		0x08c	/* Linux: bumped to submit */
#define RPIO_BLK_DONE		0x090	/* core 1: = REQ when done */
#define RPIO_BLK_STATUS		0x094	/* core 1: 0 ok */
#define RPIO_BLK_OP		0x098	/* 1 read, 2 write */
#define RPIO_BLK_NSEG		0x09c
#define RPIO_BLK_SECTOR_LO	0x0a0
#define RPIO_BLK_SECTOR_HI	0x0a4
#define RPIO_BLK_READS		0x0a8
#define RPIO_BLK_WRITES		0x0ac
#define RPIO_BLK_ERRORS		0x0b0
#define RPIO_BLK_CLOCK		0x0b4	/* the bus clock (Hz) */
#define RPIO_BLK_BUSY_US	0x0b8	/* core 1's time in requests */
#define RPIO_BLK_BYTES		0x0bc	/* bytes moved */
#define RPIO_BLK_SEGS		0x0c0	/* [addr, len] x RPIO_BLK_MAX_SEGS */
#define RPIO_BLK_MAX_SEGS	8
#define RPIO_BLK_OP_READ	1
#define RPIO_BLK_OP_WRITE	2

/* flash control (0x100) */
#define RPIO_FL_ENTRY		0x100	/* int entry(u32 op, u32 addr, u32 0) */
#define RPIO_FL_START		0x104
#define RPIO_FL_END		0x108
#define RPIO_FL_ERASES		0x10c
#define RPIO_FL_PAGES		0x110
#define RPIO_FL_STALL_MAX_US	0x114
#define RPIO_FL_STALL_TOTAL_MS	0x118
#define RPIO_FL_PAGE_BUF	0x200
#define RPIO_FL_OP_ERASE	1	/* the 4 KiB sector at addr */
#define RPIO_FL_OP_PROGRAM	2	/* the 256-byte page at addr, from PAGE_BUF */

/* core 1's counters: passes, passes with USB / PIO0 / doorbell / tick
 * pending, the last failed join's status, gSPI timeouts, ... */
#define RPIO_DBG		0x3800
#define RPIO_DBG_WORDS		10
#define RPIO_DBG_SD_XFER_US	(RPIO_DBG + 0x1c)	/* SD: time moving data blocks */
/* the SD card's error counters (the boot/sdcard crate): CRC errors,
 * written blocks refused, timeouts, requests that failed every try, card
 * initialisations, the longest busy wait (us), the last error (kind |
 * byte << 8: sd.rs err_code), requests refused for their buffers */
#define RPIO_DBG_SD_CRC		(RPIO_DBG + 0x28)
#define RPIO_DBG_SD_REJECTS	(RPIO_DBG + 0x2c)
#define RPIO_DBG_SD_TIMEOUTS	(RPIO_DBG + 0x30)
#define RPIO_DBG_SD_FAILED	(RPIO_DBG + 0x34)
#define RPIO_DBG_SD_INITS	(RPIO_DBG + 0x38)
#define RPIO_DBG_SD_BUSY_MAX_US	(RPIO_DBG + 0x3c)
#define RPIO_DBG_SD_LAST_ERR	(RPIO_DBG + 0x40)
#define RPIO_DBG_SD_BAD_REQ	(RPIO_DBG + 0x44)
#define RPIO_DBG_SD_SLOT_WAITS	(RPIO_DBG + 0x48)	/* read blocks that waited for a slot */

/* SD read slots: core 1 never writes Linux's RAM. It receives each block
 * of a read into one of RPIO_BLK_RD_SLOTS slots here, then sets the slot's
 * destination (an address in the request's buffers) and the block's CRC16
 * and bumps RD_FILLED; Linux copies the block out, checks the copy against
 * the CRC and bumps RD_COPIED. Core 1 fills a slot only once it is copied.
 * Linux sets RD_ON at probe (while it is 0, core 1 writes the blocks
 * straight into the request's buffers). */
#define RPIO_BLK_RD		0x3900
#define RPIO_BLK_RD_FILLED	(RPIO_BLK_RD + 0x00)	/* core 1 */
#define RPIO_BLK_RD_COPIED	(RPIO_BLK_RD + 0x04)	/* Linux */
#define RPIO_BLK_RD_ON		(RPIO_BLK_RD + 0x08)	/* Linux: 1 */
#define RPIO_BLK_RD_DEST	(RPIO_BLK_RD + 0x10)	/* core 1: [slot] */
#define RPIO_BLK_RD_CRC		(RPIO_BLK_RD + 0x20)	/* core 1: [slot] */
#define RPIO_BLK_RD_SLOT	0x3a00			/* 512 bytes each */
#define RPIO_BLK_RD_SLOTS	3
#define RPIO_BLK_RD_SLOT_LEN	512

/* doorbell bits: core 1 to core 0 */
#define RPIO_BELL_NET		BIT(0)
#define RPIO_BELL_BLK		BIT(1)
/* core 0 to core 1 */
#define RPIO_BELL1_NET		BIT(0)
#define RPIO_BELL1_BLK		BIT(1)

/* SIO doorbells (nommu: the physical address is the virtual one) */
#define RP2350_SIO_DOORBELL_OUT_SET	((void __iomem *)0xd0000180)
#define RP2350_SIO_DOORBELL_IN_CLR	((void __iomem *)0xd000018c)

/* ring core 1's doorbells */
static inline void rpio_ring_core1(u32 bits)
{
	wmb();
	writel(bits, RP2350_SIO_DOORBELL_OUT_SET);
}

/* take this core's doorbell bits `bits` if any is set; true if one was */
static inline bool rpio_take_bell(u32 bits)
{
	if (!(readl(RP2350_SIO_DOORBELL_IN_CLR) & bits))
		return false;
	writel(bits, RP2350_SIO_DOORBELL_IN_CLR);
	return true;
}

#endif
