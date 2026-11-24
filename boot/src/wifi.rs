// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Vanderberg
//! Wi-Fi for native Linux: the RM2
//! module's CYW43439 (stock Pico 2 W wiring: WL_ON GP23, data/IRQ GP24, CS
//! GP25, clock GP29) run by the `cyw43` driver on core 1, its Ethernet
//! frames passed to and from Linux's eth0 (rp2350-net.c) through the frame
//! rings in the I/O area (`io::RX`, `io::TX`).
//!
//! The bus is a port of `cyw43-pio` (embassy) to rp235x-hal: the same PIO
//! program and per-transaction sequence, with the CPU feeding the FIFOs where
//! the original uses DMA. There is no executor: core 1's loop
//! (`RadioService::poll`) polls the driver's futures with a no-op waker, and
//! `embassy-time` reads TIMER0 in microseconds (`io::now_us`).
//!
//! It runs on core 1 from flash (XIP), but for the bus transfers
//! (`PioSpi::read` and `write`, in SRAM). The MAC
//! and link state are published in the I/O area (`NET_RADIO`, `NET_MAC_*`,
//! `NET_LINK`); core 1 rings Linux's doorbell (`BELL_NET`) when any of it,
//! or a received frame, is new, and when a full transmit ring gets room
//! (`NET_TX_WAKE`).
//!
//! `PioSpi` (between the SPDX-Snippet lines) is ported from embassy's
//! `cyw43-pio` (cyw43-pio/src/lib.rs, https://github.com/embassy-rs/embassy):
//! its low-speed PIO program with its comments, and the `write`/`read`
//! transactions. Copyright (c) Embassy project contributors, MIT OR
//! Apache-2.0, used here under MIT: ../LICENSE-cyw43-pio. The rest of the
//! file is this project's (MIT).

use core::future::{Future, poll_fn};
use core::pin::Pin;
use core::ptr::{addr_of_mut, read_volatile, write_volatile};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use core::task::{Context, Poll, Waker};

use crate::io::{self, BELL_NET};

use cyw43::SpiBusCyw43;
use embassy_net_driver::{Driver, HardwareAddress, LinkState, RxToken, TxToken};
use hal::gpio::bank0::{Gpio23, Gpio24, Gpio25, Gpio29};
use hal::gpio::{
    FunctionNull, FunctionPio0, FunctionSioOutput, OutputDriveStrength, OutputSlewRate, Pin as GpioPin, PullDown,
    PullNone,
};
use hal::pio::{PIOBuilder, PIOExt, PinDir, PinState, SM0, ShiftDirection, StateMachine, Stopped};
use rp235x_hal as hal;
use rtt_target::rprintln;

// ------------------------------------------------------------------ time

struct UsClock;

impl embassy_time_driver::Driver for UsClock {
    fn now(&self) -> u64 {
        io::now_us()
    }
    /// Nothing to arm: every future is polled again on core 1's next pass.
    fn schedule_wake(&self, _at: u64, _waker: &Waker) {}
}

embassy_time_driver::time_driver_impl!(static CLOCK: UsClock = UsClock);

/// A no-op waker: there is no executor, every future is polled again on the
/// next pass.
fn waker() -> &'static Waker {
    Waker::noop()
}

// ------------------------------------------------------------------ PIO gSPI bus

const PIO0: usize = 0x5020_0000;
const PIO_CTRL: usize = PIO0;
const PIO_FSTAT: usize = PIO0 + 0x004;
const PIO_TXF0: usize = PIO0 + 0x010;
const PIO_RXF0: usize = PIO0 + 0x020;
const PIO_IRQ: usize = PIO0 + 0x030;
const PIO_INPUT_SYNC_BYPASS: usize = PIO0 + 0x038;
const PIO_SM0_INSTR: usize = PIO0 + 0x0d8;
/// SIO's input levels of GP0-GP31.
const SIO_GPIO_IN: usize = 0xd000_0004;
/// Atomic set / clear register aliases.
const SET: usize = 0x2000;
const CLR: usize = 0x3000;
const FSTAT_RXFULL0: u32 = 1 << 0;
const FSTAT_RXEMPTY0: u32 = 1 << 8;
const FSTAT_TXFULL0: u32 = 1 << 16;

const PIN_DIO: u8 = 24;
const PIN_CLK: u8 = 29;
/// gSPI clock for the RM2: cyw43-pio's RM2_CLOCK_DIVIDER gives a 50 MHz PIO
/// clock (25 MHz bus) at the stock 150 MHz; pick the divider that does the
/// same at our clock.
const PIO_HZ_MAX: u32 = 50_000_000;
/// A transfer that takes longer than this is abandoned (no radio fitted, or
/// it stopped answering), so core1 never hangs on the bus.
const XFER_TIMEOUT_MS: u32 = 20;

#[inline(always)]
fn rd(a: usize) -> u32 {
    // SAFETY: a PIO0 or SIO register.
    unsafe { read_volatile(a as *const u32) }
}
#[inline(always)]
fn wr(a: usize, v: u32) {
    // SAFETY: PIO0 register.
    unsafe { write_volatile(a as *mut u32, v) }
}

// SPDX-SnippetBegin
// SPDX-SnippetCopyrightText: Copyright (c) Embassy project contributors
// SPDX-License-Identifier: MIT OR Apache-2.0
pub struct PioSpi {
    cs: GpioPin<Gpio25, FunctionSioOutput, PullDown>,
    /// Owned so nothing else claims SM0; driven through its registers.
    _sm: StateMachine<(hal::pac::PIO0, SM0), Stopped>,
    _dio: GpioPin<Gpio24, FunctionPio0, PullNone>,
    _clk: GpioPin<Gpio29, FunctionPio0, PullDown>,
    timeouts: u32,
    /// `XFER_TIMEOUT_MS` in microseconds.
    timeout_us: u64,
    /// `set pindirs, 1` and `jmp wrap_target`, encoded once.
    set_pindir_out: u16,
    jmp_wrap: u16,
    /// Word reads that found the RX FIFO full, and words read.
    rx_full: u32,
    rx_words: u32,
}

impl PioSpi {
    pub fn new(
        pio0: hal::pac::PIO0,
        resets: &mut hal::pac::RESETS,
        sys_hz: u32,
        dio: GpioPin<Gpio24, FunctionNull, PullDown>,
        clk: GpioPin<Gpio29, FunctionNull, PullDown>,
        cs: GpioPin<Gpio25, FunctionNull, PullDown>,
    ) -> Self {
        let div = sys_hz.div_ceil(PIO_HZ_MAX);
        // cyw43-pio picks its program by PIO clock; below 75 MHz it is this
        // one ("low speed").
        let program = pio::pio_asm!(
            ".side_set 1"

            ".wrap_target"
            // write out x-1 bits
            "lp:"
            "out pins, 1    side 0"
            "jmp x-- lp     side 1"
            // switch directions
            "set pindirs, 0 side 0"
            "nop            side 0"
            // read in y-1 bits
            "lp2:"
            "in pins, 1     side 1"
            "jmp y-- lp2    side 0"

            // wait for event and irq host
            "wait 1 pin 0   side 0"
            "irq 0          side 0"

            ".wrap"
        );
        let (mut pio, sm0, _, _, _) = pio0.split(resets);
        let installed = pio.install(&program.program).unwrap();
        let wrap_target = installed.wrap_target();

        let mut dio = dio.into_pull_type::<PullNone>().into_function::<FunctionPio0>();
        dio.set_schmitt_enabled(true);
        dio.set_drive_strength(OutputDriveStrength::TwelveMilliAmps);
        dio.set_slew_rate(OutputSlewRate::Fast);
        let mut clk = clk.into_function::<FunctionPio0>();
        clk.set_drive_strength(OutputDriveStrength::TwelveMilliAmps);
        clk.set_slew_rate(OutputSlewRate::Fast);
        let mut cs = cs.into_push_pull_output_in_state(hal::gpio::PinState::High);
        cs.set_drive_strength(OutputDriveStrength::TwelveMilliAmps);

        let (mut sm, _rx, _tx) = PIOBuilder::from_installed_program(installed)
            .side_set_pin_base(PIN_CLK)
            .out_pins(PIN_DIO, 1)
            .in_pin_base(PIN_DIO)
            .set_pins(PIN_DIO, 1)
            .out_shift_direction(ShiftDirection::Left)
            .autopull(true)
            .in_shift_direction(ShiftDirection::Left)
            .autopush(true)
            .clock_divisor_fixed_point(div as u16, 0)
            .build(sm0);
        wr(PIO_INPUT_SYNC_BYPASS + SET, 1 << PIN_DIO);
        sm.set_pindirs([(PIN_CLK, PinDir::Output), (PIN_DIO, PinDir::Output)]);
        sm.set_pins([(PIN_CLK, PinState::Low), (PIN_DIO, PinState::Low)]);
        rprintln!("[wifi] gSPI: PIO clock {} MHz (divider {})", sys_hz / div / 1_000_000, div);
        let set_pindir_out =
            pio::InstructionOperands::SET { destination: pio::SetDestination::PINDIRS, data: 1 }.encode();
        let jmp_wrap =
            pio::InstructionOperands::JMP { address: wrap_target, condition: pio::JmpCondition::Always }.encode();
        let timeout_us = u64::from(XFER_TIMEOUT_MS) * 1000;
        PioSpi {
            cs,
            _sm: sm,
            _dio: dio,
            _clk: clk,
            timeouts: 0,
            timeout_us,
            set_pindir_out,
            jmp_wrap,
            rx_full: 0,
            rx_words: 0,
        }
    }

    #[inline(always)]
    fn set_enable(&mut self, on: bool) {
        wr(PIO_CTRL + if on { SET } else { CLR }, 1);
    }

    #[inline(always)]
    fn exec(&mut self, instr: u16) {
        wr(PIO_SM0_INSTR, u32::from(instr));
    }

    #[inline(always)]
    fn set_x(&mut self, v: u32) {
        const OUT: u16 = pio::InstructionOperands::OUT { destination: pio::OutDestination::X, bit_count: 32 }.encode();
        wr(PIO_TXF0, v);
        self.exec(OUT);
    }

    #[inline(always)]
    fn set_y(&mut self, v: u32) {
        const OUT: u16 = pio::InstructionOperands::OUT { destination: pio::OutDestination::Y, bit_count: 32 }.encode();
        wr(PIO_TXF0, v);
        self.exec(OUT);
    }

    /// Restart the program with X = bits to write - 1, Y = bits to read - 1.
    #[inline(always)]
    fn start(&mut self, write_bits: u32, read_bits: u32) {
        self.set_enable(false);
        self.set_x(write_bits);
        self.set_y(read_bits);
        self.exec(self.set_pindir_out);
        self.exec(self.jmp_wrap);
        self.set_enable(true);
    }

    /// Spin until `ready` or the transfer deadline (in microseconds); false on
    /// timeout.
    #[inline(always)]
    fn wait(&mut self, deadline: u64, ready: impl Fn() -> bool) -> bool {
        while !ready() {
            if io::now_us() > deadline {
                self.timeouts += 1;
                io::wr(io::DBG_BUS_TIMEOUTS, self.timeouts);
                return false;
            }
        }
        true
    }

    #[inline(always)]
    fn push(&mut self, deadline: u64, w: u32) -> bool {
        let ok = self.wait(deadline, || rd(PIO_FSTAT) & FSTAT_TXFULL0 == 0);
        if ok {
            wr(PIO_TXF0, w);
        }
        ok
    }

    #[inline(always)]
    fn pull(&mut self, deadline: u64) -> Option<u32> {
        self.wait(deadline, || rd(PIO_FSTAT) & FSTAT_RXEMPTY0 == 0).then(|| rd(PIO_RXF0))
    }

    /// Write data to peripheral and return status.
    // SPDX-SnippetEnd
    // `write` and `read` run from SRAM (copied with .data), with what they
    // call inlined: the CPU keeps the PIO's FIFOs fed (the original uses
    // DMA), and from flash it falls behind whenever the XIP cache is busy
    // (core 0 under network load). A read that finds the RX FIFO full has
    // stalled the state machine on the `in` that completes a word, with the
    // clock high; in high-speed mode the chip has then moved on to the next
    // bit, so the word's last bit comes back as the next word's first. Run
    // from flash, one word read in ten finds the FIFO full, and about one
    // received frame in ten fails TCP's checksum, some getting through it.
    // `DBG_SPI_RXFULL` / `DBG_SPI_WORDS` count those reads.
    // SPDX-SnippetBegin
    // SPDX-SnippetCopyrightText: Copyright (c) Embassy project contributors
    // SPDX-License-Identifier: MIT OR Apache-2.0
    #[inline(never)]
    #[unsafe(link_section = ".data.ram_func.gspi_write")]
    fn write(&mut self, write: &[u32]) -> u32 {
        self.start(write.len() as u32 * 32 - 1, 31);
        let deadline = io::now_us() + self.timeout_us;
        for &w in write {
            if !self.push(deadline, w) {
                return 0;
            }
        }
        self.pull(deadline).unwrap_or(0)
    }

    /// Send command and read response into buffer.
    #[inline(never)]
    #[unsafe(link_section = ".data.ram_func.gspi_read")]
    fn read(&mut self, cmd: u32, read: &mut [u32]) -> u32 {
        self.start(31, read.len() as u32 * 32 + 32 - 1);
        let deadline = io::now_us() + self.timeout_us;
        if !self.push(deadline, cmd) {
            return 0;
        }
        for r in read.iter_mut() {
            if rd(PIO_FSTAT) & FSTAT_RXFULL0 != 0 {
                self.rx_full = self.rx_full.wrapping_add(1);
            }
            match self.pull(deadline) {
                Some(w) => *r = w,
                None => return 0,
            }
        }
        self.rx_words = self.rx_words.wrapping_add(read.len() as u32);
        io::wr(io::DBG_SPI_RXFULL, self.rx_full);
        io::wr(io::DBG_SPI_WORDS, self.rx_words);
        self.pull(deadline).unwrap_or(0)
    }
}

// ---- receive backpressure
//
// The driver reads every frame the chip has waiting in one go (its
// `check_status` loops while the status word says F2 has a packet), but
// its channel to us holds only `RX_CHANNEL_SLOTS` frames and it drops the
// rest. A burst from the access point (an A-MPDU is often more than four
// frames) then lost frames on every TCP window, and TCP backed off: about
// 500 KiB/s into the board, with a hundred out-of-order segments per 2 MB.
// So the bus hides "F2 packet available" from the driver while the channel
// is full, and the chip keeps the frames until there's room: its status
// word ends every transaction, and the driver reads the status register
// only through it or through a plain read of that register, both of which
// pass through here. `RX_ROOM` is set by `RadioService::poll` before each
// poll of the driver, and counted down here per data frame read.

/// The cyw43 driver's receive channel (`ch::State<MTU, 4, 4>` in its lib.rs).
const RX_CHANNEL_SLOTS: u32 = 4;
/// Data frames the driver may still read before the channel is full.
static RX_ROOM: AtomicU32 = AtomicU32::new(RX_CHANNEL_SLOTS);
/// Data frames read from the chip (free-running).
static RX_READ: AtomicU32 = AtomicU32::new(0);
/// The gate hid a waiting frame during this poll of the driver
/// (`RadioService::poll` counts such polls in `DBG_NET_RX_HELD`).
static RX_HELD: AtomicBool = AtomicBool::new(false);

const STATUS_F2_PKT_AVAILABLE: u32 = 1 << 8;
/// A 32-bit read of the bus function's status register (cyw43's `cmd_word`:
/// read, incrementing, function 0, address 8, 4 bytes).
const CMD_READ_STATUS: u32 = 1 << 30 | 0x8 << 11 | 4;
/// The SDPCM header's channel (its sixth byte) for a data frame.
const SDPCM_CHANNEL_DATA: u32 = 2;

/// `status` as the driver may see it: no packet available while there's
/// no room for one. This and `rx_gate_read` are out of line, in SRAM
/// beside the transfers: inlined at each of the driver's bus calls they
/// would grow `core1_loop` (which the radio's code is inlined into, and
/// core1.x puts in SRAM) by 8 KB.
#[inline(never)]
#[unsafe(link_section = ".data.ram_func.rx_gate")]
fn rx_gate(status: u32) -> u32 {
    if status & STATUS_F2_PKT_AVAILABLE != 0 && RX_ROOM.load(Ordering::Relaxed) == 0 {
        RX_HELD.store(true, Ordering::Relaxed);
        status & !STATUS_F2_PKT_AVAILABLE
    } else {
        status
    }
}

/// After the read `cmd`: a data frame from the chip (function 2) takes a
/// slot, and a read of the status register is gated like the status word
/// that ends the transaction (returned gated).
#[inline(never)]
#[unsafe(link_section = ".data.ram_func.rx_gate_read")]
fn rx_gate_read(cmd: u32, read: &mut [u32], status: u32) -> u32 {
    let wlan_read = cmd >> 31 == 0 && (cmd >> 28) & 3 == 2;
    if wlan_read && read.len() >= 2 && (read[1] >> 8) & 0x0f == SDPCM_CHANNEL_DATA {
        let n = RX_READ.load(Ordering::Relaxed).wrapping_add(1);
        RX_READ.store(n, Ordering::Relaxed);
        io::wr(io::DBG_NET_RX_FRAMES, n);
        RX_ROOM.store(RX_ROOM.load(Ordering::Relaxed).saturating_sub(1), Ordering::Relaxed);
    }
    if cmd == CMD_READ_STATUS
        && let Some(s) = read.first_mut()
    {
        *s = rx_gate(*s);
    }
    rx_gate(status)
}

impl SpiBusCyw43 for PioSpi {
    async fn cmd_write(&mut self, write: &[u32]) -> u32 {
        self.cs.set_low_infallible();
        let status = self.write(write);
        self.cs.set_high_infallible();
        rx_gate(status)
    }

    async fn cmd_read(&mut self, write: u32, read: &mut [u32]) -> u32 {
        self.cs.set_low_infallible();
        let status = self.read(write, read);
        self.cs.set_high_infallible();
        rx_gate_read(write, read, status)
    }

    /// The chip's interrupt is the data line high while CS is up (the
    /// program, done with its transfer, has the line as an input and waits
    /// for it, then raises PIO IRQ 0). The line is read from SIO, not the
    /// IRQ flag: PIO1 and PIO2 are Linux's, and their `irq prev/next`
    /// instructions can set or clear PIO0's flags, which would forge or
    /// lose an event. The flag is still cleared, so it never stays up.
    /// While the receive channel is full there's no event: the driver
    /// couldn't take the frame (see `rx_gate`), and would spin on the line.
    async fn wait_for_event(&mut self) {
        poll_fn(|_| {
            if RX_ROOM.load(Ordering::Relaxed) != 0 && rd(SIO_GPIO_IN) & 1 << PIN_DIO != 0 {
                wr(PIO_IRQ, 1);
                Poll::Ready(())
            } else {
                Poll::Pending
            }
        })
        .await
    }
}

// SPDX-SnippetEnd

trait InfallibleOutput {
    fn set_low_infallible(&mut self);
    fn set_high_infallible(&mut self);
}

impl<P: embedded_hal::digital::OutputPin<Error = core::convert::Infallible>> InfallibleOutput for P {
    fn set_low_infallible(&mut self) {
        let Ok(()) = self.set_low();
    }
    fn set_high_infallible(&mut self) {
        let Ok(()) = self.set_high();
    }
}

// ------------------------------------------------------------------ credentials

/// Wi-Fi network to join, stored in flash (the last 4 KB below the root
/// filesystem) by tools/wifi.sh:
/// magic "WIFI", SSID length, passphrase length, SSID[32], passphrase[64].
const CREDS_ADDR: usize = 0x103F_F000;
const CREDS_MAGIC: u32 = 0x4946_4957; // "WIFI"

#[derive(Clone, Copy)]
pub struct Creds {
    ssid: &'static str,
    pass: &'static [u8],
}

impl Creds {
    pub fn from_flash() -> Option<Creds> {
        // SAFETY: flash is mapped (XIP) and never written while we run.
        let w = |i: usize| unsafe { read_volatile((CREDS_ADDR + 4 * i) as *const u32) };
        let (slen, plen) = (w(1) as usize, w(2) as usize);
        if w(0) != CREDS_MAGIC || slen == 0 || slen > 32 || plen > 64 {
            return None;
        }
        // SAFETY: as above; lengths checked.
        let (s, p) = unsafe {
            (
                core::slice::from_raw_parts((CREDS_ADDR + 12) as *const u8, slen),
                core::slice::from_raw_parts((CREDS_ADDR + 44) as *const u8, plen),
            )
        };
        Some(Creds { ssid: core::str::from_utf8(s).ok()?, pass: p })
    }

    pub fn ssid(&self) -> &str {
        self.ssid
    }
}

// ------------------------------------------------------------------ driver task

type Pwr = GpioPin<Gpio23, FunctionSioOutput, PullDown>;

static mut STATE: cyw43::State = cyw43::State::new();
/// The driver's network device once it is up (core1 only).
static mut NET: Option<cyw43::NetDriver<'static>> = None;
static READY: AtomicBool = AtomicBool::new(false);

fn net() -> Option<&'static mut cyw43::NetDriver<'static>> {
    // SAFETY: only core1's radio service touches it, from one place at a
    // time (the driver future publishes it before the service reads it).
    unsafe { (*addr_of_mut!(NET)).as_mut() }
}

/// The same bytes through flash's uncached, non-allocating XIP alias, so
/// uploading the firmware doesn't sweep Linux's data out of the XIP
/// cache.
fn uncached(b: &'static cyw43::Aligned<cyw43::A4, [u8]>) -> &'static cyw43::Aligned<cyw43::A4, [u8]> {
    const XIP_NOCACHE_NOALLOC_OFFSET: usize = 0x1400_0000 - 0x1000_0000;
    let p = core::ptr::from_ref(b);
    let a = p.cast::<u8>() as usize;
    if !(0x1000_0000..0x1400_0000).contains(&a) {
        return b;
    }
    let q = core::ptr::slice_from_raw_parts((a + XIP_NOCACHE_NOALLOC_OFFSET) as *const u8, b.len());
    // SAFETY: the same flash bytes at another address with the same
    // alignment, never written while we run; `Aligned` is repr(C) around
    // the slice, so the pointer metadata carries over.
    unsafe { &*(q as *const cyw43::Aligned<cyw43::A4, [u8]>) }
}

/// Bring the chip up, then join `creds` (retrying) while serving the bus.
async fn run(pwr: Pwr, spi: PioSpi, creds: Option<Creds>) {
    let fw = uncached(cyw43::aligned_bytes!("../firmware/43439A0.bin"));
    let clm = uncached(cyw43::aligned_bytes!("../firmware/43439A0_clm.bin"));
    let nvram = uncached(cyw43::aligned_bytes!("../firmware/nvram_rp2040.bin"));
    // SAFETY: STATE is borrowed here only, once.
    let state = unsafe { &mut *addr_of_mut!(STATE) };
    let (device, mut control, runner) = cyw43::new(state, pwr, spi, fw, nvram).await;
    // SAFETY: as in `net`.
    unsafe { *addr_of_mut!(NET) = Some(device) };
    let control_task = async move {
        control.init(clm).await;
        control.set_power_management(cyw43::PowerManagementMode::None).await;
        let mac = control.address().await;
        rprintln!(
            "[wifi] CYW43439 up, MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
        );
        READY.store(true, Ordering::Relaxed);
        let Some(c) = creds else {
            rprintln!("[wifi] no network configured (tools/wifi.sh)");
            return;
        };
        // Join, and join again when the link stays down: the chip
        // reassociates by itself after most losses, but not all (after some
        // deauths it never does, which would leave the board off the network
        // for good).
        // Failed joins back off, so an absent network costs one attempt
        // every few minutes.
        let mut wait_s = JOIN_RETRY_MIN_S;
        loop {
            // the driver waits for the chip's join events with no deadline;
            // one that never comes (it happens) would stop joining
            // for good, so a join that takes too long counts as failed
            let join = control.join(c.ssid, cyw43::JoinOptions::new(c.pass));
            let r = embassy_time::with_timeout(embassy_time::Duration::from_secs(JOIN_TIMEOUT_S), join).await;
            let r = match r {
                Ok(r) => r.map_err(|e| match e {
                    cyw43::JoinError::NetworkNotFound => 1000,
                    cyw43::JoinError::AuthenticationFailure => 1001,
                    cyw43::JoinError::JoinFailure(s) => u32::from(s),
                }),
                Err(_) => {
                    control.leave().await;
                    Err(1002)
                }
            };
            match r {
                Ok(()) => {
                    rprintln!("[wifi] joined {:?}", c.ssid);
                    // The chip drops multicast frames whose address is not
                    // in its filter (10 slots). mDNS (224.0.0.251, for
                    // mdnsd) is the only group Linux uses; adding it again
                    // after each join is a no-op, and keeps it there should
                    // a leave clear the list.
                    if let Err(e) = control.add_multicast_address(MDNS_MAC).await {
                        rprintln!("[wifi] mDNS multicast filter: {:?}", e);
                    }
                    io::wr(io::NET_JOINS, io::rd(io::NET_JOINS) + 1);
                    wait_s = JOIN_RETRY_MIN_S;
                    REJOINING.store(false, Ordering::Relaxed);
                }
                Err(e) => {
                    io::wr(io::NET_JOIN_FAILS, io::rd(io::NET_JOIN_FAILS) + 1);
                    // 1000 not found, 1001 authentication, 1002 timed out,
                    // else the chip's status
                    io::wr(io::DBG_JOIN_ERR, e);
                    rprintln!("[wifi] join {:?} failed ({}); again in {} s", c.ssid, e, wait_s);
                    embassy_time::Timer::after_secs(wait_s.into()).await;
                    wait_s = (wait_s * 2).min(JOIN_RETRY_MAX_S);
                    continue;
                }
            }
            // LINK_UP follows the driver's link state (`poll`, on this core);
            // the chip gets LINK_DOWN_REJOIN_S to recover by itself. A link
            // that is up but passes no traffic (STALLED, from `poll`) is
            // rejoined at once: the chip never reports that one itself.
            STALLED.store(false, Ordering::Relaxed);
            let mut down_s = 0;
            while down_s < LINK_DOWN_REJOIN_S {
                embassy_time::Timer::after_secs(1).await;
                if STALLED.swap(false, Ordering::Relaxed) {
                    break;
                }
                down_s = if LINK_UP.load(Ordering::Relaxed) { 0 } else { down_s + 1 };
            }
            REJOINS.fetch_add(1, Ordering::Relaxed);
            io::wr(io::NET_REJOINS, REJOINS.load(Ordering::Relaxed));
            if down_s < LINK_DOWN_REJOIN_S {
                rprintln!("[wifi] link up but nothing received for {} s; joining again", TRAFFIC_STALL_S);
            } else {
                rprintln!("[wifi] link down for {} s; joining again", LINK_DOWN_REJOIN_S);
            }
            // the driver keeps reporting the link up through `leave` (it
            // changes the link state on the chip's events only): Linux
            // sees it down from here until the join succeeds
            REJOINING.store(true, Ordering::Relaxed);
            control.leave().await;
        }
    };
    embassy_futures::join::join(runner.run(), control_task).await;
}

/// The RM2's pins, as `hal::gpio::Pins::new` hands them out.
pub struct RadioPins {
    pub pwr: GpioPin<Gpio23, FunctionNull, PullDown>,
    pub dio: GpioPin<Gpio24, FunctionNull, PullDown>,
    pub cs: GpioPin<Gpio25, FunctionNull, PullDown>,
    pub clk: GpioPin<Gpio29, FunctionNull, PullDown>,
}

/// The driver future for `RadioService` (set up on core0, run on core1).
pub fn driver(
    pio0: hal::pac::PIO0,
    resets: &mut hal::pac::RESETS,
    sys_hz: u32,
    pins: RadioPins,
    creds: Option<Creds>,
) -> impl Future<Output = ()> + use<> {
    let spi = PioSpi::new(pio0, resets, sys_hz, pins.dio, pins.clk, pins.cs);
    let pwr = pins.pwr.into_push_pull_output_in_state(hal::gpio::PinState::Low);
    run(pwr, spi, creds)
}

/// Linux's address and gateway, from a DHCP ACK in `f` (if it is one).
#[inline(never)]
fn cold_learn_lease(f: &[u8], lease: &mut ([u8; 4], [u8; 4])) {
    if let Some(l) = dhcp_ack(f)
        && l != *lease
    {
        *lease = l;
        let (a, g) = l;
        rprintln!(
            "[wifi] keepalive: eth0 {}.{}.{}.{}, gateway {}.{}.{}.{}",
            a[0], a[1], a[2], a[3], g[0], g[1], g[2], g[3]
        );
    }
}

/// A DHCP ACK (server port 67 to client port 68): the address it gives and
/// its first router.
#[inline(always)]
fn dhcp_ack(f: &[u8]) -> Option<([u8; 4], [u8; 4])> {
    if f.len() < 14 + 20 || f[12] != 0x08 || f[13] != 0x00 {
        return None;
    }
    let ip = &f[14..];
    let ihl = usize::from(ip[0] & 0x0F) * 4;
    if ip[0] >> 4 != 4 || ihl < 20 || ip[9] != 17 || ip.len() < ihl + 8 {
        return None;
    }
    let udp = &ip[ihl..];
    if udp[0] != 0 || udp[1] != 67 || udp[2] != 0 || udp[3] != 68 {
        return None;
    }
    let b = &udp[8..];
    if b.len() < 240 || b[0] != 2 || b[236..240] != [99, 130, 83, 99] {
        return None;
    }
    let addr = [b[16], b[17], b[18], b[19]];
    let (mut ack, mut router) = (false, None);
    let mut i = 240;
    while i < b.len() {
        match b[i] {
            0 => i += 1,
            255 => break,
            code => {
                let len = usize::from(*b.get(i + 1)?);
                let v = b.get(i + 2..i + 2 + len)?;
                match code {
                    53 if len == 1 => ack = v[0] == 5,
                    3 if len >= 4 => router = Some([v[0], v[1], v[2], v[3]]),
                    _ => {}
                }
                i += 2 + len;
            }
        }
    }
    if !ack || addr == [0; 4] {
        return None;
    }
    Some((addr, router?))
}

// ------------------------------------------------------------------ Linux's link (shared with core0)


pub const RADIO_STARTING: u32 = 0;
pub const RADIO_UP: u32 = 1;
pub const RADIO_ABSENT: u32 = 2;
/// `RADIO_*`, mirrored at `io::NET_RADIO`; the MAC there is valid once it
/// is `RADIO_UP`.
pub static RADIO: AtomicU32 = AtomicU32::new(RADIO_STARTING);
/// Mirrored at `io::NET_LINK`.
pub static LINK_UP: AtomicBool = AtomicBool::new(false);
/// Seconds the link may be down (after a join) before the firmware leaves
/// and joins again: room for the chip's own reassociation (a roam, the
/// access point restarting its radio).
const LINK_DOWN_REJOIN_S: u32 = 30;
/// A failed join is tried again after this, doubling up to the max.
const JOIN_RETRY_MIN_S: u32 = 5;
/// A join that hasn't finished after this has failed.
const JOIN_TIMEOUT_S: u64 = 20;
const JOIN_RETRY_MAX_S: u32 = 300;
/// The Ethernet address of the mDNS group 224.0.0.251.
const MDNS_MAC: [u8; 6] = [0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb];
/// Seconds Linux may go on sending while no unicast frame comes back
/// before the link counts as stalled. After a deauth the chip sometimes
/// reports the link up while nothing passes either way; only traffic shows
/// it. The keepalive (`KEEPALIVE_S`) gets a unicast reply on a working link,
/// so this never fires there.
const TRAFFIC_STALL_S: u32 = 30;
/// Every this many seconds, while the link is up, the firmware sends an ARP
/// request for Linux's gateway as Linux (its address and gateway
/// come from the DHCP ACK it received): the reply keeps a working link from
/// looking stalled, and a dead link gets something to leave unanswered.
const KEEPALIVE_S: u32 = 20;
/// How long Linux may leave a full receive ring alone before frames are
/// thrown away instead of held in the chip: well past a 4 KiB flash erase
/// (about 45 ms with core 0's interrupts off).
const LINUX_RX_STALL_US: u64 = 200_000;
/// Set by `poll` when the link stalls; the control task rejoins.
static STALLED: AtomicBool = AtomicBool::new(false);
/// Set by the control task from `leave` until the next join succeeds;
/// `LINK_UP` is false meanwhile.
static REJOINING: AtomicBool = AtomicBool::new(false);
/// Joins after the first, for diagnostics.
pub static REJOINS: AtomicU32 = AtomicU32::new(0);

/// core1's radio service: drives the driver, moves frames between it and
/// the rings, publishes the MAC and the link state.
pub struct RadioService<F> {
    /// Pinned in place: the service never moves (core 1 pins it on its stack).
    fut: F,
    /// When bring-up started, and how long it may take.
    t0: u64,
    bring_up_us: u64,
    /// When Linux first sent since the last unicast frame came in (0:
    /// nothing unanswered), and how long that may last (`TRAFFIC_STALL_S`).
    unanswered_since: u64,
    stall_us: u64,
    /// Linux's address and gateway from its last DHCP ACK (0: none yet),
    /// and when the next keepalive goes.
    lease: ([u8; 4], [u8; 4]),
    next_keepalive: u64,
    keepalive_us: u64,
    /// Data frames taken from the driver's channel (free-running, against
    /// `RX_READ`), and frames thrown away because Linux took none.
    rx_taken: u32,
    rx_discarded: u32,
    /// Polls in which the gate left a frame in the chip.
    rx_held: u32,
    /// The last drain couldn't see the channel: `receive` needs a free
    /// transmit slot too, and there was none (see `set_rx_room`).
    rx_unknown: bool,
    /// Linux's full receive ring: its head when first seen full, and when
    /// (0: it had room last time).
    rx_head: u32,
    rx_head_at: u64,
}

// SAFETY: built on core 0 before core 1 starts, then used by core 1 only.
unsafe impl<F> Send for RadioService<F> {}

impl<F: Future<Output = ()>> RadioService<F> {
    /// `bring_up_ms`: how long the chip may take to come up before the
    /// eth0 is reported absent.
    pub fn new(fut: F, bring_up_ms: u32) -> Self {
        RadioService {
            fut,
            t0: 0,
            bring_up_us: u64::from(bring_up_ms) * 1000,
            unanswered_since: 0,
            stall_us: u64::from(TRAFFIC_STALL_S) * 1_000_000,
            lease: ([0; 4], [0; 4]),
            next_keepalive: 0,
            keepalive_us: u64::from(KEEPALIVE_S) * 1_000_000,
            rx_taken: 0,
            rx_discarded: 0,
            rx_held: 0,
            rx_unknown: false,
            rx_head: 0,
            rx_head_at: 0,
        }
    }

    /// Room in the driver's receive channel, for `rx_gate`: its slots less
    /// the data frames read and not yet taken. When the last drain couldn't
    /// see the channel (the transmit side was full), the gate stays open:
    /// the driver reads, and may drop frames past a full channel as it did
    /// before the gate, because the transmit side drains only on the credit
    /// the chip sends with what is read, and a closed gate would wait on it
    /// for ever.
    #[inline(always)]
    fn set_rx_room(&self) {
        let queued = RX_READ.load(Ordering::Relaxed).wrapping_sub(self.rx_taken).min(RX_CHANNEL_SLOTS);
        let room = if self.rx_unknown { RX_CHANNEL_SLOTS } else { RX_CHANNEL_SLOTS - queued };
        RX_ROOM.store(room, Ordering::Relaxed);
    }

    /// With Linux's receive ring full: whether Linux has left it alone
    /// for `LINUX_RX_STALL_US` (eth0 down, or not up yet). Frames are then
    /// thrown away, as the driver used to, so they can't hold back the
    /// chip's events and ioctl replies behind them.
    #[inline(always)]
    fn linux_rx_stalled(&mut self) -> bool {
        let (head, now) = (io::rd(io::NET_RX_HEAD), io::now_us());
        if head != self.rx_head || self.rx_head_at == 0 {
            self.rx_head = head;
            self.rx_head_at = now;
            return false;
        }
        now - self.rx_head_at > LINUX_RX_STALL_US
    }

    /// The keepalive: a broadcast ARP request for the gateway, from the
    /// radio's MAC and Linux's address. It counts as Linux sending.
    #[inline(never)]
    fn cold_keepalive(&mut self, d: &mut cyw43::NetDriver<'static>, cx: &mut Context<'_>) {
        let HardwareAddress::Ethernet(mac) = d.hardware_address() else { return };
        let Some(tx) = d.transmit(cx) else { return };
        let (ip, gw) = self.lease;
        tx.consume(60, |f| {
            f.fill(0);
            f[0..6].fill(0xFF);
            f[6..12].copy_from_slice(&mac);
            f[12..22].copy_from_slice(&[0x08, 0x06, 0, 1, 0x08, 0x00, 6, 4, 0, 1]);
            f[22..28].copy_from_slice(&mac);
            f[28..32].copy_from_slice(&ip);
            f[38..42].copy_from_slice(&gw);
        });
        let now = io::now_us();
        self.next_keepalive = now + self.keepalive_us;
        if self.unanswered_since == 0 {
            self.unanswered_since = now;
        }
    }

    /// Inlined into `poll`, which core1.x puts in SRAM: the driver's
    /// runner is polled from here.
    #[inline(always)]
    fn poll_driver(&mut self) {
        let mut cx = Context::from_waker(waker());
        // SAFETY: `fut` never moves (see the field).
        let _ = unsafe { Pin::new_unchecked(&mut self.fut) }.poll(&mut cx);
    }
}

impl<F: Future<Output = ()>> RadioService<F> {
    pub fn poll(&mut self) {
        let mut cx = Context::from_waker(waker());
        // Linux -> radio: as many frames as the driver takes. A frame
        // Linux has queued starts the stall clock, sent or not.
        if !io::TX.is_empty() && self.unanswered_since == 0 {
            self.unanswered_since = io::now_us();
        }
        if let Some(d) = net() {
            while !io::TX.is_empty() {
                let Some(tx) = d.transmit(&mut cx) else { break };
                io::TX.pop_with(|f| tx.consume(f.len(), |b| b.copy_from_slice(f)));
                if io::rd(io::NET_TX_WAKE) != 0 {
                    io::wr(io::NET_TX_WAKE, 0);
                    io::ring_other(BELL_NET);
                }
            }
        }
        self.set_rx_room();
        self.poll_driver();
        if RX_HELD.swap(false, Ordering::Relaxed) {
            self.rx_held = self.rx_held.wrapping_add(1);
            io::wr(io::DBG_NET_RX_HELD, self.rx_held);
        }
        if RADIO.load(Ordering::Relaxed) == RADIO_STARTING && !READY.load(Ordering::Relaxed) {
            let now = io::now_us();
            if self.t0 == 0 {
                self.t0 = now;
            } else if now - self.t0 > self.bring_up_us {
                rprintln!("[wifi] radio did not come up; eth0 stays down");
                RADIO.store(RADIO_ABSENT, Ordering::Release);
                io::wr(io::NET_RADIO, RADIO_ABSENT);
                io::ring_other(BELL_NET);
            }
            return;
        }
        let Some(d) = net() else { return };
        // radio -> Linux, while there's room (the rest waits in the driver,
        // and then in the chip: `rx_gate`)
        let mut got = false;
        self.rx_unknown = false;
        loop {
            let room = io::RX.has_room();
            if room {
                self.rx_head_at = 0;
            } else if !self.linux_rx_stalled() {
                break;
            }
            let Some((rx, _tx)) = d.receive(&mut cx) else {
                // `receive` also needs a free transmit slot
                if d.transmit(&mut cx).is_some() {
                    // the channel is empty: whatever `RX_READ` counted that
                    // never reached it (a frame the driver couldn't parse)
                    // is gone
                    self.rx_taken = RX_READ.load(Ordering::Relaxed);
                } else {
                    self.rx_unknown = true;
                }
                break;
            };
            self.rx_taken = self.rx_taken.wrapping_add(1);
            if !room {
                rx.consume(|_| ());
                self.rx_discarded = self.rx_discarded.wrapping_add(1);
                io::wr(io::DBG_NET_RX_DISCARDED, self.rx_discarded);
                continue;
            }
            let answered = &mut self.unanswered_since;
            let lease = &mut self.lease;
            io::RX.push_with(|slot| {
                rx.consume(|b| {
                    // a unicast frame (to us): the link passes traffic
                    if b.first().is_some_and(|&a| a & 1 == 0) {
                        *answered = 0;
                    }
                    // DHCP to the client port (IPv4 without options, UDP
                    // 67 to 68): learn the lease for the keepalive
                    if b.len() > 42 && b[12] == 8 && b[13] == 0 && b[14] == 0x45 && b[23] == 17 && b[35] == 67 && b[37] == 68 {
                        cold_learn_lease(b, lease);
                    }
                    let n = b.len().min(slot.len());
                    slot[..n].copy_from_slice(&b[..n]);
                    n
                })
            });
            got = true;
        }
        if RADIO.load(Ordering::Relaxed) == RADIO_STARTING {
            // READY: the chip is up and the driver knows its MAC
            if let HardwareAddress::Ethernet(m) = d.hardware_address() {
                io::wr(io::NET_MAC_LO, u32::from_le_bytes([m[0], m[1], m[2], m[3]]));
                io::wr(io::NET_MAC_HI, u32::from_le_bytes([m[4], m[5], 0, 0]));
            }
            RADIO.store(RADIO_UP, Ordering::Release);
            core::sync::atomic::fence(Ordering::SeqCst);
            io::wr(io::NET_RADIO, RADIO_UP);
            got = true;
        }
        let up = d.link_state(&mut cx) == LinkState::Up && !REJOINING.load(Ordering::Relaxed);
        if up != LINK_UP.load(Ordering::Relaxed) {
            LINK_UP.store(up, Ordering::Release);
            io::wr(io::NET_LINK, u32::from(up));
            got = true;
        }
        if up && io::rd(io::NET_CMD) == io::NET_CMD_REJOIN {
            // Linux asked for a rejoin (/sys/class/net/eth0/rejoin): handled
            // as a stalled link, so it tests that path
            io::wr(io::NET_CMD, 0);
            self.unanswered_since = 0;
            STALLED.store(true, Ordering::Relaxed);
        } else if !up {
            // a link that is down is the control task's to handle
            self.unanswered_since = 0;
        } else if self.unanswered_since != 0 && io::now_us() - self.unanswered_since > self.stall_us {
            self.unanswered_since = 0;
            STALLED.store(true, Ordering::Relaxed);
        } else if self.lease.0 != [0; 4] && self.lease.1 != [0; 4] && io::now_us() >= self.next_keepalive {
            self.cold_keepalive(d, &mut cx);
        }
        if got {
            io::ring_other(BELL_NET);
        }
    }
}
