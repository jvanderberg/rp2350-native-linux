//! What core 1 shares with Linux on core 0, besides the console rings:
//! the I/O area in SRAM (`IO`, 16 KiB at 0x20068000), the SIO doorbells and
//! the park handshake for flash writes.
//!
//! The layout is mirrored in linux/drivers/rp2350/rp2350-io.h;
//! change both together. Every field is a little-endian u32. Indices run
//! free (index % slots is the slot). Each side writes only its own fields.
//!
//! | Offset | What |
//! |---|---|
//! | 0x000 | magic "RPIO", version |
//! | 0x040 | network control (`NET_*`) |
//! | 0x080 | SD card control (`BLK_*`), its segment list at 0x0c0 |
//! | 0x100 | flash control (`FL_*`) |
//! | 0x200 | flash page buffer (256 bytes) |
//! | 0x400 | the flash routine's stack (1 KiB, top at 0x800) |
//! | 0x800 | network receive ring (core 1 to Linux), `NET_SLOTS` slots |
//! | 0x2000 | network transmit ring (Linux to core 1) |
//! | 0x3800 | core 1's counters (`DBG_*`) |
//! | 0x3900 | SD read slots' control (`BLK_RD_*`), the slots at 0x3a00 |
//!
//! Doorbells: core 1 rings core 0's (`ring_core0`, SIO_IRQ_BELL, IRQ 26
//! on core 0) with `BELL_NET` or `BELL_BLK` when it hands Linux something;
//! Linux rings core 1's with `BELL1_*` when it queues work, which wakes
//! core 1 from `wfi`.

use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{fence, AtomicBool, Ordering};

pub const IO: usize = 0x2006_8000;
pub const IO_SIZE: usize = 0x4000;
pub const IO_MAGIC: u32 = 0x4f49_5052; // "RPIO"
pub const IO_VERSION: u32 = 1;

// network control
pub const NET: usize = IO + 0x40;
pub const NET_RADIO: usize = NET; // 0 starting, 1 up, 2 absent
pub const NET_LINK: usize = NET + 0x04;
pub const NET_MAC_LO: usize = NET + 0x08;
pub const NET_MAC_HI: usize = NET + 0x0c;
pub const NET_RX_HEAD: usize = NET + 0x10; // Linux
pub const NET_RX_TAIL: usize = NET + 0x14; // core 1
pub const NET_TX_HEAD: usize = NET + 0x18; // core 1
pub const NET_TX_TAIL: usize = NET + 0x1c; // Linux
pub const NET_TX_WAKE: usize = NET + 0x20; // Linux sets, core 1 clears (and rings)
pub const NET_REJOINS: usize = NET + 0x24;
pub const NET_JOINS: usize = NET + 0x28; // successful joins
pub const NET_JOIN_FAILS: usize = NET + 0x2c; // failed attempts
pub const NET_CMD: usize = NET + 0x30; // Linux sets, core 1 clears
pub const NET_CMD_REJOIN: u32 = 1; // leave and join again now
pub const NET_SLOTS: u32 = 4;
pub const NET_SLOT: usize = 0x600;
/// Slot: u32 length, then the frame.
pub const NET_FRAME_MAX: usize = NET_SLOT - 4;
pub const NET_RX_RING: usize = IO + 0x800;
pub const NET_TX_RING: usize = IO + 0x2000;

// SD card control
pub const BLK: usize = IO + 0x80;
pub const BLK_SECTORS_LO: usize = BLK; // core 1: 0 while no card
pub const BLK_SECTORS_HI: usize = BLK + 0x04;
pub const BLK_GEN: usize = BLK + 0x08; // core 1: bumped on a change of card
pub const BLK_REQ: usize = BLK + 0x0c; // Linux: bumped to submit
pub const BLK_DONE: usize = BLK + 0x10; // core 1: = BLK_REQ when done
pub const BLK_STATUS: usize = BLK + 0x14; // core 1: 0 ok
pub const BLK_OP: usize = BLK + 0x18; // 1 read, 2 write
pub const BLK_NSEG: usize = BLK + 0x1c;
pub const BLK_SECTOR_LO: usize = BLK + 0x20;
pub const BLK_SECTOR_HI: usize = BLK + 0x24;
pub const BLK_READS: usize = BLK + 0x28;
pub const BLK_WRITES: usize = BLK + 0x2c;
pub const BLK_ERRORS: usize = BLK + 0x30;
pub const BLK_CLOCK: usize = BLK + 0x34; // the bus clock, Hz (actual)
pub const BLK_BUSY_US: usize = BLK + 0x38; // time in requests
pub const BLK_BYTES: usize = BLK + 0x3c; // bytes moved
pub const BLK_SEGS: usize = BLK + 0x40; // [addr, len] x BLK_MAX_SEGS
pub const BLK_MAX_SEGS: usize = 8;

// flash control
pub const FL: usize = IO + 0x100;
pub const FL_ENTRY: usize = FL; // the routine (Thumb address)
pub const FL_START: usize = FL + 0x04; // writable region [start, end)
pub const FL_END: usize = FL + 0x08;
pub const FL_ERASES: usize = FL + 0x0c;
pub const FL_PAGES: usize = FL + 0x10;
pub const FL_STALL_MAX_US: usize = FL + 0x14;
pub const FL_STALL_TOTAL_MS: usize = FL + 0x18;
pub const FL_PAGE_BUF: usize = IO + 0x200;
pub const FL_STACK_TOP: usize = IO + 0x800;

// core 1's counters (diagnostics; rp2350-net shows them as core1_stats)
pub const DBG: usize = IO + 0x3800;
pub const DBG_PASSES: usize = DBG; // loop passes
pub const DBG_WAKE_USB: usize = DBG + 0x04; // passes with each source pending
pub const DBG_WAKE_PIO: usize = DBG + 0x08;
pub const DBG_WAKE_BELL: usize = DBG + 0x0c;
pub const DBG_WAKE_TICK: usize = DBG + 0x10;
pub const DBG_JOIN_ERR: usize = DBG + 0x14; // the last failed join's status
pub const DBG_BUS_TIMEOUTS: usize = DBG + 0x18; // gSPI transfers abandoned
pub const DBG_SD_XFER_US: usize = DBG + 0x1c; // SD: time moving data blocks on the bus
pub const DBG_SPI_RXFULL: usize = DBG + 0x20; // gSPI reads that found the RX FIFO full
pub const DBG_SPI_WORDS: usize = DBG + 0x24; // gSPI words read
// the SD card's (sdcard's SdStats; rp2350-sd shows them in stats)
pub const DBG_SD_CRC: usize = DBG + 0x28; // CRC errors, both ways
pub const DBG_SD_REJECTS: usize = DBG + 0x2c; // written blocks the card refused
pub const DBG_SD_TIMEOUTS: usize = DBG + 0x30; // waits for the card that ran out
pub const DBG_SD_FAILED: usize = DBG + 0x34; // requests that failed every try
pub const DBG_SD_INITS: usize = DBG + 0x38; // card initialisations
pub const DBG_SD_BUSY_MAX_US: usize = DBG + 0x3c; // longest busy wait
pub const DBG_SD_LAST_ERR: usize = DBG + 0x40; // kind | byte << 8 (sd.rs, err_code)
pub const DBG_SD_BAD_REQ: usize = DBG + 0x44; // requests refused unread (bad buffers)
pub const DBG_SD_SLOT_WAITS: usize = DBG + 0x48; // read blocks that waited for a free slot
// Wi-Fi receive (wifi.rs, rx_gate; rp2350-net shows them in radio_stats)
pub const DBG_NET_RX_FRAMES: usize = DBG + 0x4c; // data frames read from the chip
pub const DBG_NET_RX_HELD: usize = DBG + 0x50; // polls of the driver that left a frame in the chip
pub const DBG_NET_RX_DISCARDED: usize = DBG + 0x54; // frames thrown away: Linux took none

// SD read slots (sd.rs recv_block): core 1 fills them, Linux copies them out
pub const BLK_RD: usize = IO + 0x3900;
pub const BLK_RD_FILLED: usize = BLK_RD; // core 1: blocks handed over
pub const BLK_RD_COPIED: usize = BLK_RD + 0x04; // Linux: blocks copied out
pub const BLK_RD_ON: usize = BLK_RD + 0x08; // Linux: 1 = it copies read blocks out
pub const BLK_RD_DEST: usize = BLK_RD + 0x10; // core 1: [slot] where the block goes
pub const BLK_RD_CRC: usize = BLK_RD + 0x20; // core 1: [slot] its CRC16
pub const BLK_RD_SLOT: usize = IO + 0x3a00; // the slots, 512 bytes each
pub const BLK_RD_SLOTS: usize = 3;

// doorbells (bit numbers are the same on both sides)
pub const BELL_NET: u32 = 1 << 0;
pub const BELL_BLK: u32 = 1 << 1;
#[allow(dead_code)]
pub const BELL1_NET: u32 = 1 << 0;
#[allow(dead_code)]
pub const BELL1_BLK: u32 = 1 << 1;
pub const BELL1_PARK: u32 = 1 << 2;

const SIO: usize = 0xd000_0000;
const DOORBELL_OUT_SET: usize = SIO + 0x180;
pub const DOORBELL_IN_CLR: usize = SIO + 0x18c;
const TIMERAWH: usize = 0x400b_0024;
const TIMERAWL: usize = 0x400b_0028;

#[inline(always)]
pub fn rd(a: usize) -> u32 {
    // SAFETY: fixed RP2350 addresses (registers, the I/O area, flash).
    unsafe { read_volatile(a as *const u32) }
}
#[inline(always)]
pub fn wr(a: usize, v: u32) {
    // SAFETY: as rd.
    unsafe { write_volatile(a as *mut u32, v) }
}

/// Microseconds since boot: TIMER0, which Linux also runs on (read only here).
#[inline(always)]
pub fn now_us() -> u64 {
    loop {
        let (hi, lo, hi2) = (rd(TIMERAWH), rd(TIMERAWL), rd(TIMERAWH));
        if hi == hi2 {
            return (u64::from(hi) << 32) | u64::from(lo);
        }
    }
}

/// Ring the other core's doorbells `bits` (from either core).
#[inline]
pub fn ring_other(bits: u32) {
    fence(Ordering::SeqCst);
    wr(DOORBELL_OUT_SET, bits);
}

/// Zero the area and put the header down (core 0, before core 1 and Linux).
pub fn init() {
    for a in (IO..IO + IO_SIZE).step_by(4) {
        wr(a, 0);
    }
    wr(IO + 4, IO_VERSION);
    fence(Ordering::SeqCst);
    wr(IO, IO_MAGIC);
}

// ---- park: core 1 off XIP while core 0 writes flash -----------------------------

pub static PARK_REQ: AtomicBool = AtomicBool::new(false);
pub static PARK_ACK: AtomicBool = AtomicBool::new(false);

/// Core 1, at the top of its loop: if core 0 asked, wait in SRAM until it
/// lets go.
#[inline(always)]
pub fn serve_park() {
    if PARK_REQ.load(Ordering::Acquire) {
        parked();
    }
}

/// A leaf in SRAM: no flash or PSRAM access until core 0 is done.
#[inline(never)]
#[unsafe(link_section = ".data.ram_func.parked")]
fn parked() {
    PARK_ACK.store(true, Ordering::Release);
    while PARK_REQ.load(Ordering::Acquire) {
        core::hint::spin_loop();
    }
    PARK_ACK.store(false, Ordering::Release);
}

// ---- frame rings ------------------------------------------------------------------

/// One side of a frame ring in the I/O area.
pub struct Ring {
    head: usize,
    tail: usize,
    base: usize,
}

pub const RX: Ring = Ring { head: NET_RX_HEAD, tail: NET_RX_TAIL, base: NET_RX_RING };
pub const TX: Ring = Ring { head: NET_TX_HEAD, tail: NET_TX_TAIL, base: NET_TX_RING };

impl Ring {
    pub fn is_empty(&self) -> bool {
        rd(self.head) == rd(self.tail)
    }
    /// Producer: a free slot?
    pub fn has_room(&self) -> bool {
        rd(self.tail).wrapping_sub(rd(self.head)) < NET_SLOTS
    }
    /// Producer: fill the next slot (`fill` returns the length).
    pub fn push_with(&self, fill: impl FnOnce(&mut [u8]) -> usize) -> bool {
        if !self.has_room() {
            return false;
        }
        let t = rd(self.tail);
        let slot = self.base + (t % NET_SLOTS) as usize * NET_SLOT;
        // SAFETY: a free slot of the ring, which the consumer leaves alone
        // until the tail moves past it.
        let buf = unsafe { core::slice::from_raw_parts_mut((slot + 4) as *mut u8, NET_FRAME_MAX) };
        let n = fill(buf).min(NET_FRAME_MAX);
        wr(slot, n as u32);
        fence(Ordering::SeqCst);
        wr(self.tail, t.wrapping_add(1));
        true
    }
    /// Consumer: hand the oldest frame to `take`, then free its slot.
    pub fn pop_with<R>(&self, take: impl FnOnce(&[u8]) -> R) -> Option<R> {
        let h = rd(self.head);
        if rd(self.tail) == h {
            return None;
        }
        fence(Ordering::SeqCst);
        let slot = self.base + (h % NET_SLOTS) as usize * NET_SLOT;
        let n = (rd(slot) as usize).min(NET_FRAME_MAX);
        // SAFETY: a published slot; the producer leaves it alone until the
        // head moves past it.
        let r = take(unsafe { core::slice::from_raw_parts((slot + 4) as *const u8, n) });
        fence(Ordering::SeqCst);
        wr(self.head, h.wrapping_add(1));
        Some(r)
    }
}
