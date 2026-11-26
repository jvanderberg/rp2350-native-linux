//! The SD card for Linux (linux/drivers/rp2350/rp2350-sd.c):
//! the card on SPI0 or SPI1, on the pins the board config gives (MISO,
//! pulled up; the chip select, a GPIO; SCK; MOSI), run by `sdcard`
//! (sdcard/) (commands with CRCs, retries, insertion and removal) on core 1.
//!
//! Linux posts one request at a time in the I/O area (`BLK_*`): a sector,
//! read or write, and up to `BLK_MAX_SEGS` buffers in its RAM. For a write,
//! core 1 sends straight from those buffers (PSRAM, through the XIP cache the
//! cores share). For a read it never writes them: each block goes to one of
//! `BLK_RD_SLOTS` slots in the I/O area, and Linux copies it out (see
//! `recv_block`). Then core 1 sets `BLK_DONE` and rings `BELL_BLK`. The card's size is `BLK_SECTORS_*`
//! (0 while there is none); a change of card bumps `BLK_GEN` and rings too.
//!
//! The bus (`BoardSd`) uses TIMER0 as its clock: the
//! transfers drive the PL022's registers, keeping its FIFOs full. It is set
//! up by register too (`BoardSd::new`, as rp235x-hal's spi.rs does it), so
//! that the block and the pins can be chosen at run time.

use rp235x_hal as hal;
use sdcard::{SdCard, SdError, crc16, crc16_byte};

use crate::io::{self, *};
use crate::pins;

const SPI0_BASE: usize = 0x4008_0000;
const SPI1_BASE: usize = 0x4008_8000;
const SSPCR0: usize = 0x00;
const SSPCR1: usize = 0x04;
const SSPDR_OFF: usize = 0x08;
const SSPSR_OFF: usize = 0x0c;
const SSPCPSR: usize = 0x10;
const SSPDMACR: usize = 0x24;
const CR1_SSE: u32 = 1 << 1;
/// 8-bit frames, Motorola format, mode 0 (CPOL 0, CPHA 0)
const CR0_DSS_8: u32 = 7;
const CR0_SCR_LSB: u32 = 8;
/// The block's data and status registers, set by `BoardSd::new` (before
/// core 1 runs; read only after).
static mut SSPDR: *mut u32 = (SPI1_BASE + SSPDR_OFF) as *mut u32;
static mut SSPSR: *const u32 = (SPI1_BASE + SSPSR_OFF) as *const u32;
const SR_TNF: u32 = 1 << 1;
const SR_RNE: u32 = 1 << 2;
const SR_BSY: u32 = 1 << 4;
const FIFO: usize = 8;

/// A read slot holds one data block.
const RD_SLOT_LEN: usize = 512;

/// Linux's RAM: the only place request buffers may be.
const RAM_START: u32 = 0x1140_0000;
const RAM_END: u32 = 0x11c0_0000;

/// The card's pins: MISO, CS, SCK, MOSI (config.rs `sd`).
#[derive(Clone, Copy)]
pub struct SdPins(pub [u8; 4]);

pub struct BoardSd {
    base: usize,
    cs: u8,
    peri_hz: u32,
    /// The rate the PL022 really runs at (clk_peri over its dividers)
    actual_hz: u32,
    /// Read blocks handed to Linux (`BLK_RD_FILLED`), and the CRC of the
    /// one in the slot being filled
    rd_filled: u32,
    rd_crc: u16,
}

impl BoardSd {
    /// The SPI block of the pins (config.rs checked they share one), reset
    /// and set up at 400 kHz, and the pins on it.
    pub fn new(p: SdPins, resets: &mut hal::pac::RESETS, peri_hz: u32) -> Self {
        let [miso, cs, sck, mosi] = p.0;
        let block = pins::spi_block(sck);
        let base = if block == 0 { SPI0_BASE } else { SPI1_BASE };
        resets.reset().modify(|_, w| if block == 0 { w.spi0().set_bit() } else { w.spi1().set_bit() });
        resets.reset().modify(|_, w| if block == 0 { w.spi0().clear_bit() } else { w.spi1().clear_bit() });
        while !(if block == 0 { resets.reset_done().read().spi0().bit() } else { resets.reset_done().read().spi1().bit() }) {}
        // SAFETY: core 0, before core 1 runs; read only from then on.
        unsafe {
            SSPDR = (base + SSPDR_OFF) as *mut u32;
            SSPSR = (base + SSPSR_OFF) as *const u32;
        }
        let mut b = BoardSd { base, cs, peri_hz, actual_hz: 0, rd_filled: rd(BLK_RD_FILLED), rd_crc: 0 };
        b.actual_hz = b.set_baudrate(400_000);
        wr(base + SSPCR0, (rd(base + SSPCR0) & !0xff) | CR0_DSS_8);
        wr(base + SSPDMACR, 3);
        wr(base + SSPCR1, rd(base + SSPCR1) | CR1_SSE);
        pins::output(cs, true);
        pins::set_function(miso, pins::FUNC_SPI);
        pins::set_pull(miso, true, false);
        for p in [sck, mosi] {
            pins::set_function(p, pins::FUNC_SPI);
            pins::set_pull(p, false, false);
        }
        b
    }

    /// The card's pins back to how reset left them (after a test in setup).
    pub fn release(p: SdPins) {
        for pin in p.0 {
            pins::release(pin);
        }
    }

    // SPDX-SnippetBegin
    // SPDX-SnippetCopyrightText: Copyright (c) 2021-2024 The rp-rs Developers
    // SPDX-License-Identifier: MIT OR Apache-2.0
    /// rp235x-hal's `Spi::set_baudrate` (spi.rs), on this block's registers.
    fn set_baudrate(&mut self, baudrate: u32) -> u32 {
        let freq_in = self.peri_hz;
        let mut prescale: u8 = u8::MAX;
        let mut postdiv: u8 = 0;

        // Find smallest prescale value which puts output frequency in range of
        // post-divide. Prescale is an even number from 2 to 254 inclusive.
        for prescale_option in (2u32..=254).step_by(2) {
            // We need to use an saturating_mul here because with a high baudrate certain invalid prescale
            // values might not fit in u32. However we can be sure those values exceed the max sys_clk frequency
            // So clamping a u32::MAX is fine here...
            if freq_in < ((prescale_option + 2) * 256).saturating_mul(baudrate) {
                prescale = prescale_option as u8;
                break;
            }
        }

        // Find largest post-divide which makes output <= baudrate. Post-divide is
        // an integer in the range 0 to 255 inclusive.
        for postdiv_option in (1..=255u8).rev() {
            if freq_in / (prescale as u32 * postdiv_option as u32) > baudrate {
                postdiv = postdiv_option;
                break;
            }
        }

        wr(self.base + SSPCPSR, u32::from(prescale));
        wr(self.base + SSPCR0, (rd(self.base + SSPCR0) & !(0xff << CR0_SCR_LSB)) | u32::from(postdiv) << CR0_SCR_LSB);

        // Return the frequency we were able to achieve
        freq_in / (prescale as u32 * (1 + postdiv as u32))
    }
    // SPDX-SnippetEnd

    #[inline(always)]
    fn run(n: usize, mut tx: impl FnMut(usize) -> u8, mut rx: impl FnMut(usize, u8)) {
        // SAFETY: the block's data and status registers; the block is ours,
        // and the pointers were set before core 1 started.
        unsafe {
            let (dr, sr_reg) = (SSPDR, SSPSR);
            while sr_reg.read_volatile() & SR_RNE != 0 {
                let _ = dr.read_volatile();
            }
            let (mut sent, mut got) = (0, 0);
            while got < n {
                let sr = sr_reg.read_volatile();
                if sent < n && sent - got < FIFO && sr & SR_TNF != 0 {
                    dr.write_volatile(u32::from(tx(sent)));
                    sent += 1;
                }
                if sr & SR_RNE != 0 {
                    rx(got, dr.read_volatile() as u8);
                    got += 1;
                }
            }
            while sr_reg.read_volatile() & SR_BSY != 0 {}
        }
    }
}

impl BoardSd {
    /// A data block of a request from Linux, which takes read blocks
    /// through the slots
    fn slot_block(&self, buf: &[u8]) -> bool {
        buf.len() == RD_SLOT_LEN && (RAM_START..RAM_END).contains(&(buf.as_ptr() as u32)) && rd(BLK_RD_ON) == 1
    }
}

impl sdcard::SdBus for BoardSd {
    fn set_hz(&mut self, hz: u32) {
        self.actual_hz = self.set_baudrate(hz);
    }
    fn select(&mut self, on: bool) {
        pins::set(self.cs, !on);
    }
    fn transfer(&mut self, buf: &mut [u8]) {
        let p = buf.as_mut_ptr();
        // SAFETY: `i` < buf.len(); a byte is read before it is overwritten.
        Self::run(buf.len(), |i| unsafe { p.add(i).read() }, |i, b| unsafe { p.add(i).write(b) });
    }
    fn send(&mut self, tx: &[u8]) {
        let t0 = xfer_start(tx.len());
        Self::run(tx.len(), |i| tx[i], |_, _| {});
        xfer_end(t0);
    }
    fn send_block(&mut self, blk: &[u8]) -> u16 {
        // the CRC of each byte as it goes out: Linux may write the buffer
        // meanwhile (a process woken while it is swapped out), so each byte
        // is read once (volatile), and the CRC is of what the card got
        let t0 = xfer_start(blk.len());
        let p = blk.as_ptr();
        let mut crc = 0;
        // SAFETY: `i` < blk.len()
        Self::run(
            blk.len(),
            |i| {
                let b = unsafe { p.add(i).read_volatile() };
                crc = crc16_byte(crc, b);
                b
            },
            |_, _| {},
        );
        xfer_end(t0);
        crc
    }
    fn recv_block(&mut self, buf: &mut [u8]) -> u16 {
        // A block for Linux goes to a free slot in SRAM, not to its buffer:
        // core 1 never writes Linux's RAM (PSRAM, through the XIP cache).
        // Linux cleans and invalidates that cache line by line, as two
        // operations (rp2350-atrans.c, xip_flush_all, at each switch between
        // ATRANS processes), so a line core 1 wrote between the two would be
        // dropped, and could be dropped after any check core 1 made. With
        // only core 0 dirtying PSRAM lines, that flush can't lose anything.
        // Linux copies each block out of its slot (rp2350-sd.c).
        if !self.slot_block(buf) {
            // the card's registers and status, into core 1's own buffers;
            // or a kernel without the slots (BLK_RD_ON 0): straight in
            self.recv(buf);
            return crc16(buf);
        }
        // a slot Linux has copied (the card waits meanwhile: no clock)
        if self.rd_filled.wrapping_sub(rd(BLK_RD_COPIED)) >= BLK_RD_SLOTS as u32 {
            wr(DBG_SD_SLOT_WAITS, rd(DBG_SD_SLOT_WAITS).wrapping_add(1));
            while self.rd_filled.wrapping_sub(rd(BLK_RD_COPIED)) >= BLK_RD_SLOTS as u32 {
                // Linux writes flash with interrupts off, and waits for
                // core 1 to park: it can't copy until then
                if io::PARK_REQ.load(core::sync::atomic::Ordering::Acquire) {
                    io::serve_park();
                }
            }
        }
        let slot = BLK_RD_SLOT + RD_SLOT_LEN * (self.rd_filled as usize % BLK_RD_SLOTS);
        let t0 = xfer_start(RD_SLOT_LEN);
        let p = slot as *mut u8;
        let mut crc = 0;
        // SAFETY: `i` < RD_SLOT_LEN, in the slot (I/O area, SRAM)
        Self::run(RD_SLOT_LEN, |_| 0xFF, |i, x| unsafe {
            p.add(i).write_volatile(x);
            crc = crc16_byte(crc, x);
        });
        xfer_end(t0);
        self.rd_crc = crc;
        crc
    }
    fn block_ok(&mut self, buf: &mut [u8]) {
        if !self.slot_block(buf) {
            return;
        }
        let a = buf.as_ptr() as u32;
        // hand the slot to Linux: where it goes, its CRC (Linux checks its
        // copy against it), then the count
        let k = self.rd_filled as usize % BLK_RD_SLOTS;
        wr(BLK_RD_DEST + 4 * k, a);
        wr(BLK_RD_CRC + 4 * k, u32::from(self.rd_crc));
        self.rd_filled = self.rd_filled.wrapping_add(1);
        core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
        wr(BLK_RD_FILLED, self.rd_filled);
        io::ring_other(BELL_BLK);
    }
    fn recv(&mut self, rx: &mut [u8]) {
        let t0 = xfer_start(rx.len());
        Self::run(rx.len(), |_| 0xFF, |i, b| rx[i] = b);
        xfer_end(t0);
    }
    fn now_us(&mut self) -> u64 {
        io::now_us()
    }
}

/// Data blocks (512 bytes and up) are timed: `DBG_SD_XFER_US` is the time
/// spent moving data on the bus, apart from commands, tokens, CRCs and busy.
#[inline(always)]
fn xfer_start(n: usize) -> u64 {
    if n >= 512 { io::now_us() } else { 0 }
}
#[inline(always)]
fn xfer_end(t0: u64) {
    if t0 != 0 {
        wr(DBG_SD_XFER_US, rd(DBG_SD_XFER_US).wrapping_add((io::now_us() - t0) as u32));
    }
}

pub struct SdService {
    card: SdCard<BoardSd>,
    media_gen: u32,
    bad_requests: u32,
}

/// `DBG_SD_LAST_ERR`: the kind, and the byte some kinds carry, << 8.
fn err_code(e: Option<SdError>) -> u32 {
    match e {
        None => 0,
        Some(SdError::NoResponse) => 1,
        Some(SdError::R1(b)) => 2 | u32::from(b) << 8,
        Some(SdError::Timeout) => 3,
        Some(SdError::Crc) => 4,
        Some(SdError::Token(b)) => 5 | u32::from(b) << 8,
        Some(SdError::Write(b)) => 6 | u32::from(b) << 8,
        Some(SdError::Init) => 7,
        Some(SdError::Unsupported) => 8,
    }
}

impl SdService {
    /// Looks for a card at once (core 0, at boot), so one that is in is there
    /// when Linux probes.
    pub fn new(bus: BoardSd) -> Self {
        let mut card = SdCard::new(bus, sdcard::HIGH_HZ);
        card.start();
        let mut s = SdService { card, media_gen: 0, bad_requests: 0 };
        s.publish();
        s
    }

    fn publish(&mut self) {
        let n = self.card.sectors();
        wr(BLK_SECTORS_LO, n as u32);
        wr(BLK_SECTORS_HI, (n >> 32) as u32);
        wr(BLK_CLOCK, if n != 0 { self.card.bus.actual_hz } else { 0 });
        self.media_gen = self.media_gen.wrapping_add(1);
        core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
        wr(BLK_GEN, self.media_gen);
    }

    pub fn present(&self) -> bool {
        self.card.present()
    }

    /// One pass: a request, if Linux posted one; otherwise the card check
    /// (about once a second, inside `poll`).
    pub fn poll(&mut self) {
        let req = rd(BLK_REQ);
        if req != rd(BLK_DONE) {
            core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
            let t0 = io::now_us();
            let ok = self.request();
            wr(BLK_BUSY_US, rd(BLK_BUSY_US).wrapping_add((io::now_us() - t0) as u32));
            wr(BLK_STATUS, u32::from(!ok));
            let st = &self.card.stats;
            wr(BLK_READS, st.reads);
            wr(BLK_WRITES, st.writes);
            wr(BLK_ERRORS, st.errors);
            wr(DBG_SD_CRC, st.crc_errors);
            wr(DBG_SD_REJECTS, st.write_rejects);
            wr(DBG_SD_TIMEOUTS, st.timeouts);
            wr(DBG_SD_FAILED, st.failed);
            wr(DBG_SD_INITS, st.inits);
            wr(DBG_SD_BUSY_MAX_US, st.busy_us_max);
            wr(DBG_SD_LAST_ERR, err_code(st.last_error));
            wr(DBG_SD_BAD_REQ, self.bad_requests);
            // the clock can drop to the default speed after CRC errors
            wr(BLK_CLOCK, self.card.bus.actual_hz);
            core::sync::atomic::fence(core::sync::atomic::Ordering::SeqCst);
            wr(BLK_DONE, req);
            io::ring_other(BELL_BLK);
            return;
        }
        if self.card.poll() {
            self.publish();
            io::ring_other(BELL_BLK);
        }
    }

    fn request(&mut self) -> bool {
        let op = rd(BLK_OP);
        let nseg = rd(BLK_NSEG) as usize;
        let sector = u64::from(rd(BLK_SECTOR_LO)) | u64::from(rd(BLK_SECTOR_HI)) << 32;
        if nseg == 0 || nseg > BLK_MAX_SEGS {
            self.bad_requests += 1;
            return false;
        }
        let mut segs: [&mut [u8]; BLK_MAX_SEGS] = Default::default();
        for (i, s) in segs.iter_mut().enumerate().take(nseg) {
            let a = rd(BLK_SEGS + 8 * i);
            let l = rd(BLK_SEGS + 8 * i + 4);
            if a < RAM_START || l == 0 || !l.is_multiple_of(512) || a.checked_add(l).is_none_or(|e| e > RAM_END) {
                self.bad_requests += 1;
                return false;
            }
            // SAFETY: a buffer in Linux's RAM (checked above) that Linux
            // leaves alone until the request is done.
            *s = unsafe { core::slice::from_raw_parts_mut(a as *mut u8, l as usize) };
            wr(BLK_BYTES, rd(BLK_BYTES).wrapping_add(l));
        }
        match op {
            1 => self.card.read_segs(sector, &mut segs[..nseg]),
            2 => {
                let ro: [&[u8]; BLK_MAX_SEGS] = core::array::from_fn(|i| &*segs[i]);
                self.card.write_segs(sector, &ro[..nseg])
            }
            _ => false,
        }
    }
}
