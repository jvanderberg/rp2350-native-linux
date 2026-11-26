//! RP2350 board support: clocks, PSRAM, TIMER0, BOOTSEL.
//!
//! - Clocks: one of `CPU_PRESETS` (the board config picks it): voltage
//!   first, then flash timing that is safe at every preset, then the PLL
//!   (the order PicoMite uses), then the flash at the preset's speed.
//! - PSRAM on QMI CS1, on whichever of GP0, GP8, GP19 or GP47 the board
//!   config says, or the first that answers (AP Memory APS6404L). The sequence follows
//!   PicoMite/FanPico psram.c (Timo Kokkonen): exit QPI, read ID, reset,
//!   enter QPI, program M1 timing/format.
//! - TIMER0.DBGPAUSE, which otherwise stops the timer whenever a debugger
//!   halts a core.
//!
//! LICENSE UNRESOLVED. The PSRAM code here (the CMD_ and KGD_ constants,
//! the csr_* and direct_mode_* helpers, psram_read_id, psram_qmi_setup,
//! psram_init's timing arithmetic and detect_size) follows FanPico's
//! src/psram.c (https://github.com/tjko/fanpico) closely, by way of a
//! separate PSRAM bring-up test: the same functions, names, constants and
//! steps.
//!   Copyright (C) 2025 Timo Kokkonen <tjko@iki.fi>
//!   License: GPL-3.0-or-later
//! If it is a derived work, those parts are GPL-3.0-or-later, not MIT. This
//! is open for the owner to settle; until then this file carries no SPDX
//! line of its own.
//!
//! `vreg_set_voltage` is a port of the pico-sdk's (src/rp2_common/hardware_vreg,
//! https://github.com/raspberrypi/pico-sdk), Copyright (c) 2020 Raspberry Pi
//! (Trading) Ltd., BSD-3-Clause; the license text is
//! ../LICENSE-BSD-3-Clause.

use core::arch::asm;
use core::ptr::{read_volatile, write_volatile};

use rp235x_hal as hal;

pub const XTAL_HZ: u32 = 12_000_000;

/// A CPU speed: the core voltage and the system PLL (12 MHz crystal x
/// `vco_mhz / 12`, then / `pd1` / `pd2`). clk_sys = clk_peri = `mhz`.
#[derive(Clone, Copy)]
pub struct CpuPreset {
    pub mhz: u32,
    /// POWMAN VREG VSEL: 0.55 V + 0.05 V per step (0b01011 = 1.10 V, the
    /// chip's default; at most 0b01111 = 1.30 V without unlocking more)
    pub vsel: u32,
    pub mv: u32,
    pub vco_mhz: u32,
    pub pd1: u8,
    pub pd2: u8,
}

/// The CPU speeds setup offers: the chip's stock 150 MHz first, up to the
/// 348 MHz this board has run at since bring-up (exact 350 is not reachable
/// from a 12 MHz crystal). Each one is tested on the board
/// (docs/boot-and-io.md, "CPU speed").
pub const CPU_PRESETS: [CpuPreset; 5] = [
    CpuPreset { mhz: 150, vsel: 0b01011, mv: 1100, vco_mhz: 1500, pd1: 5, pd2: 2 },
    CpuPreset { mhz: 200, vsel: 0b01011, mv: 1100, vco_mhz: 1200, pd1: 6, pd2: 1 },
    CpuPreset { mhz: 250, vsel: 0b01100, mv: 1150, vco_mhz: 1500, pd1: 6, pd2: 1 },
    CpuPreset { mhz: 300, vsel: 0b01100, mv: 1150, vco_mhz: 1500, pd1: 5, pd2: 1 },
    CpuPreset { mhz: 348, vsel: 0b01101, mv: 1200, vco_mhz: 1044, pd1: 3, pd2: 1 },
];
const _: () = {
    let mut i = 0;
    while i < CPU_PRESETS.len() {
        let p = CPU_PRESETS[i];
        assert!(p.vco_mhz.is_multiple_of(12) && p.vco_mhz / p.pd1 as u32 / p.pd2 as u32 == p.mhz);
        assert!(p.vco_mhz >= 750 && p.vco_mhz <= 1600 && p.pd1 >= p.pd2 && p.vsel <= 0b01111);
        i += 1;
    }
};

/// The flash's and the PSRAM's SCK: clk_sys over the smallest divider that
/// gives at most 116 MHz (the PSRAM is specified to 109 MHz; it and the
/// flash have run at 116 MHz at 348 / 3 since bring-up).
const QMI_SCK_MAX: u32 = 116_000_000;
pub fn qmi_clkdiv(sys_hz: u32) -> u32 {
    sys_hz.div_ceil(QMI_SCK_MAX)
}

// ---- registers (values from the Pico SDK rp2350 headers) -------------------

const SET: usize = 0x2000;
const CLR: usize = 0x3000;

const QMI: usize = 0x400d_0000;
const DIRECT_CSR: usize = QMI;
const DIRECT_TX: usize = QMI + 0x04;
const DIRECT_RX: usize = QMI + 0x08;
const M0_TIMING: usize = QMI + 0x0c;
const M1_TIMING: usize = QMI + 0x20;
const M1_RFMT: usize = QMI + 0x24;
const M1_RCMD: usize = QMI + 0x28;
const M1_WFMT: usize = QMI + 0x2c;
const M1_WCMD: usize = QMI + 0x30;

const CSR_EN: u32 = 1 << 0;
const CSR_BUSY: u32 = 1 << 1;
const CSR_ASSERT_CS1N: u32 = 1 << 3;
const CSR_TXEMPTY: u32 = 1 << 11;
const CSR_CLKDIV_LSB: u32 = 22;
const TX_OE: u32 = 1 << 19;
const TX_IWIDTH_Q: u32 = 2 << 16;

/// TIMER0.DBGPAUSE: resets to 7, which pauses the timer whenever a core is
/// halted by the debugger and skews every timing below. Cleared at start.
const TIMER0_DBGPAUSE: usize = 0x400b_002c;

const XIP_CTRL: usize = 0x400c_8000;

mod powman {
    pub const VREG_CTRL: usize = 0x4010_0004;
    pub const VREG: usize = 0x4010_000c;
    pub const PASSWORD: u32 = 0x5afe_0000;
    pub const VREG_CTRL_UNLOCK: u32 = 1 << 13;
    pub const VREG_UPDATE_IN_PROGRESS: u32 = 1 << 15;
    pub const VREG_VSEL_LSB: u32 = 4;
    pub const VREG_VSEL_BITS: u32 = 0x1f0;
}
const XIP_CTRL_WRITABLE_M1: u32 = 1 << 11;

/// M1_RFMT: quad prefix/addr/dummy/data, 8-bit prefix, 24 dummy bits.
const RFMT: u32 = 6 << 16 | 1 << 12 | 2 << 8 | 2 << 6 | 2 << 4 | 2 << 2 | 2;
/// M1_WFMT: same without dummy bits.
const WFMT: u32 = 1 << 12 | 2 << 8 | 2 << 6 | 2 << 4 | 2 << 2 | 2;
// The values a C implementation of the same sequence programs (read back
// from the target over SWD).
const _: () = assert!(RFMT == 0x0006_12aa && WFMT == 0x0000_12aa);

const CMD_QUAD_READ: u32 = 0xeb;
const CMD_QUAD_WRITE: u32 = 0x38;
const CMD_ENTER_QPI: u32 = 0x35;
const CMD_EXIT_QPI: u32 = 0xf5;
const CMD_RESET_ENABLE: u32 = 0x66;
const CMD_RESET: u32 = 0x99;
const CMD_READ_ID: u32 = 0x9f;
const KGD_PASS: u8 = 0x5d;

pub const PSRAM_UNCACHED: usize = 0x1500_0000;
const PSRAM_WINDOW: u32 = 16 << 20;
const PSRAM_MAX_CSR_CLK: u32 = 5_000_000;

// ---- raw register access (always inlined so RAM functions stay in RAM) -----

#[inline(always)]
fn rd(addr: usize) -> u32 {
    // SAFETY: fixed, valid RP2350 peripheral or PSRAM address.
    unsafe { read_volatile(addr as *const u32) }
}

#[inline(always)]
fn wr(addr: usize, v: u32) {
    // SAFETY: fixed, valid RP2350 peripheral or PSRAM address.
    unsafe { write_volatile(addr as *mut u32, v) }
}

/// Order the register writes before later fetches and accesses.
#[inline(always)]
fn barrier() {
    // SAFETY: memory ordering barriers only.
    unsafe {
        asm!("dsb sy", "isb sy")
    };
}

#[inline(always)]
fn csr_wait(mask: u32, want: u32) {
    while rd(DIRECT_CSR) & mask != want {}
}

/// One direct-mode transfer with CS1 asserted; returns the received byte.
#[inline(always)]
fn csr_xfer(tx: u32) -> u32 {
    wr(DIRECT_TX, tx);
    csr_wait(CSR_TXEMPTY, CSR_TXEMPTY);
    csr_wait(CSR_BUSY, 0);
    rd(DIRECT_RX)
}

#[inline(always)]
fn csr_cmd(cmd: u32) {
    wr(DIRECT_CSR + SET, CSR_ASSERT_CS1N);
    csr_xfer(cmd);
    wr(DIRECT_CSR + CLR, CSR_ASSERT_CS1N);
}

#[inline(always)]
fn direct_mode_on(csr_clkdiv: u32) {
    wr(DIRECT_CSR, csr_clkdiv << CSR_CLKDIV_LSB | CSR_EN);
    csr_wait(CSR_BUSY, 0);
}

/// Leave direct mode and wait until the QMI has actually seen it. An
/// instruction fetch can reach the QMI ahead of this store; a flash fetch that
/// lands while direct mode is still enabled is a bus error (an instruction
/// access fault on return to flash code).
#[inline(always)]
fn direct_mode_off() {
    wr(DIRECT_CSR + CLR, CSR_EN | CSR_ASSERT_CS1N);
    while rd(DIRECT_CSR) & (CSR_EN | CSR_BUSY) != 0 {}
    barrier();
}

// The functions below put the QMI into direct mode or change flash timing, so
// they must not fetch from flash: they live in SRAM and call nothing.
// Interrupts are never enabled in this program, so no masking is needed.

#[inline(never)]
#[unsafe(link_section = ".data.ram_func")]
fn psram_read_id(csr_clkdiv: u32) -> [u8; 8] {
    let mut id = [0u8; 8];
    direct_mode_on(csr_clkdiv);
    csr_cmd(TX_OE | TX_IWIDTH_Q | CMD_EXIT_QPI);
    wr(DIRECT_CSR + SET, CSR_ASSERT_CS1N);
    let mut i = 0;
    while i < 12 {
        let rx = csr_xfer(if i == 0 { CMD_READ_ID } else { 0 });
        if i >= 4 {
            id[i - 4] = rx as u8;
        }
        i += 1;
    }
    direct_mode_off();
    id
}

#[inline(never)]
#[unsafe(link_section = ".data.ram_func")]
fn psram_qmi_setup(csr_clkdiv: u32, timing: u32) {
    direct_mode_on(csr_clkdiv);
    csr_cmd(CMD_RESET_ENABLE);
    csr_cmd(CMD_RESET);
    csr_cmd(CMD_ENTER_QPI);
    direct_mode_off();
    wr(M1_TIMING, timing);
    wr(M1_RFMT, RFMT);
    wr(M1_RCMD, CMD_QUAD_READ);
    wr(M1_WFMT, WFMT);
    wr(M1_WCMD, CMD_QUAD_WRITE);
    wr(XIP_CTRL + SET, XIP_CTRL_WRITABLE_M1);
    // Read back so every write above has landed before we return to flash.
    rd(M1_WCMD);
    rd(XIP_CTRL);
    barrier();
}

/// Flash (M0) timing valid at every preset: COOLDOWN=1, RXDELAY=2,
/// MIN_DESELECT=6, CLKDIV=4 (87 MHz at 348; PicoMite's value above 288 MHz).
const M0_TIMING_SLOW: u32 = 0x4000_6204;
/// Flash (M0) timing at `sys_hz`: COOLDOWN=1, MIN_DESELECT=6, CLKDIV from
/// `qmi_clkdiv`, and RXDELAY (half system clocks) about 5.7 ns. At 348 MHz
/// that is 0x4000_6403: CLKDIV=3, 116 MHz, the PSRAM's clock on the same
/// bus; RXDELAY 2 to 7 all read the flash correctly there and 1 does not,
/// and 4 is the PSRAM's value too. Kernel text misses are flash reads, so
/// this takes about 17% off a pipe round trip at 348 MHz
/// (docs/performance.md, "Flash").
fn m0_timing_fast(sys_hz: u32) -> u32 {
    let clkdiv = qmi_clkdiv(sys_hz);
    let rxdelay = (sys_hz / 1_000_000 * 114).div_ceil(10_000).min(2 * clkdiv - 1);
    1 << 30 | 6 << 12 | rxdelay << 8 | clkdiv
}

#[inline(never)]
#[unsafe(link_section = ".data.ram_func")]
fn flash_timing(t: u32) {
    wr(M0_TIMING, t);
    rd(M0_TIMING);
    for _ in 0..1000 {
        // SAFETY: no-op.
        unsafe { asm!("nop") };
    }
}

// SPDX-SnippetBegin
// SPDX-SnippetCopyrightText: Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
// SPDX-License-Identifier: BSD-3-Clause
/// Port of the Pico SDK's vreg_set_voltage for RP2350 (POWMAN).
fn vreg_set_voltage(vsel: u32) {
    use powman::*;
    wr(VREG_CTRL + SET, PASSWORD | VREG_CTRL_UNLOCK);
    while rd(VREG) & VREG_UPDATE_IN_PROGRESS != 0 {}
    let cur = rd(VREG) & 0xffff & !VREG_VSEL_BITS;
    wr(VREG, PASSWORD | cur | vsel << VREG_VSEL_LSB);
    while rd(VREG) & VREG_UPDATE_IN_PROGRESS != 0 {}
}
// SPDX-SnippetEnd

/// Bring up the clocks at `cpu` (see the module docs) and return the clock
/// manager.
pub fn init_clocks(
    cpu: &CpuPreset,
    xosc: hal::pac::XOSC,
    clocks: hal::pac::CLOCKS,
    pll_sys: hal::pac::PLL_SYS,
    pll_usb: hal::pac::PLL_USB,
    resets: &mut hal::pac::RESETS,
    watchdog: &mut hal::Watchdog,
) -> hal::clocks::ClocksManager {
    use fugit::RateExtU32;
    use hal::pll::{common_configs::PLL_USB_48MHZ, setup_pll_blocking};
    let xosc = hal::xosc::setup_xosc_blocking(xosc, XTAL_HZ.Hz()).unwrap();
    watchdog.enable_tick_generation((XTAL_HZ / 1_000_000) as u16);
    vreg_set_voltage(cpu.vsel);
    flash_timing(M0_TIMING_SLOW);
    let mut clocks = hal::clocks::ClocksManager::new(clocks);
    let pll = hal::pll::PLLConfig {
        vco_freq: cpu.vco_mhz.MHz(),
        refdiv: 1,
        post_div1: cpu.pd1,
        post_div2: cpu.pd2,
    };
    let pll_sys = setup_pll_blocking(pll_sys, xosc.operating_frequency(), pll, &mut clocks, resets).unwrap();
    let pll_usb =
        setup_pll_blocking(pll_usb, xosc.operating_frequency(), PLL_USB_48MHZ, &mut clocks, resets)
            .unwrap();
    clocks.init_default(&xosc, &pll_sys, &pll_usb).unwrap();
    flash_timing(m0_timing_fast(cpu.mhz * 1_000_000));
    clocks
}

/// Keep TIMER0 running while a debugger halts a core (DBGPAUSE resets to 7).
pub fn timer_ignore_debug_halt() {
    wr(TIMER0_DBGPAUSE, 0);
}

// What the bring-up found, whole; the stub reads only some of it.
#[allow(dead_code)]
#[derive(Clone, Copy, Debug)]
pub struct Psram {
    /// The chip select's pin.
    pub cs: u8,
    /// mfid, kgd, eid[6]
    pub id: [u8; 8],
    /// Detected size in bytes.
    pub size: u32,
    pub clkdiv: u32,
    pub sck_hz: u32,
    /// M1_TIMING as programmed.
    pub timing: u32,
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug)]
pub enum PsramError {
    /// No chip answered (known-good-die byte wrong) on any pin tried.
    NotFound,
}

/// The PSRAM's ID if one answers with its chip select on `cs` (one of
/// `pins::CS1_PINS`); the pin is left as the chip select if so, and
/// released if not.
pub fn psram_probe(cs: u8, sys_hz: u32) -> Option<[u8; 8]> {
    crate::pins::set_function(cs, crate::pins::FUNC_XIP_CS1);
    let id = psram_read_id(sys_hz.div_ceil(PSRAM_MAX_CSR_CLK));
    if id[1] == KGD_PASS {
        Some(id)
    } else {
        crate::pins::release(cs);
        None
    }
}

/// Bring up the PSRAM with its chip select on `cs`, or on the first of
/// `candidates` that answers if `cs` is None, and map it writable at
/// 0x11000000 (cached) / `PSRAM_UNCACHED`.
pub fn psram_init(cs: Option<u8>, candidates: &[u8], sys_hz: u32) -> Result<Psram, PsramError> {
    let tries = cs.as_ref().map(core::slice::from_ref).unwrap_or(candidates);
    let Some((cs, id)) = tries.iter().find_map(|&p| psram_probe(p, sys_hz).map(|id| (p, id))) else {
        return Err(PsramError::NotFound);
    };
    let csr_clkdiv = sys_hz.div_ceil(PSRAM_MAX_CSR_CLK);
    // AP Memory APS6404L: 109 MHz max, tCEM 8 us, tCPH 18 ns.
    let period_fs = 1_000_000_000_000_000u64 / u64::from(sys_hz);
    let clkdiv = qmi_clkdiv(sys_hz);
    let max_select = ((8_000_000_000u64 >> 6) / period_fs) as u32;
    let min_deselect = 18_000_000u64.div_ceil(period_fs) as u32;
    let rxdelay = 1 + if sys_hz > 150_000_000 { clkdiv } else { 1 };
    let timing = 1 << 30 | 2 << 28 | 3 << 23 | max_select << 17 | min_deselect << 12 | rxdelay << 8 | clkdiv;
    psram_qmi_setup(csr_clkdiv, timing);
    Ok(Psram { cs, id, size: detect_size(), clkdiv, sck_hz: sys_hz / clkdiv, timing: rd(M1_TIMING) })
}

/// Smallest power of two at which the PSRAM address space wraps.
fn detect_size() -> u32 {
    let mut sz = 1u32 << 20;
    while sz < PSRAM_WINDOW {
        wr(PSRAM_UNCACHED, 0);
        wr(PSRAM_UNCACHED + sz as usize, 0xc0ff_ee42);
        if rd(PSRAM_UNCACHED) == 0xc0ff_ee42 {
            wr(PSRAM_UNCACHED, 0);
            return sz;
        }
        wr(PSRAM_UNCACHED + sz as usize, 0);
        sz <<= 1;
    }
    PSRAM_WINDOW
}

// ---- USB BOOTSEL -------------------------------------------------------------

/// Watchdog SCRATCH0 (kept across a debugger reset; the bootrom's reboot
/// handling uses SCRATCH4..7).
const WATCHDOG_SCRATCH0: usize = 0x400d_800c;
/// A debugger writes this to SCRATCH0 and resets the chip to ask the
/// firmware for USB BOOTSEL (see `bootsel_if_requested`).
pub const BOOTSEL_MAGIC: u32 = 0xb007_5e1c;

/// Reboot into the bootrom's USB BOOTSEL mode (picotool / mass storage).
pub fn reboot_to_bootsel() -> ! {
    hal::reboot::reboot(
        hal::reboot::RebootKind::BootSel { picoboot_disabled: false, msd_disabled: false },
        hal::reboot::RebootArch::Arm,
    )
}

/// Reset core 1 (PSM FRCE_OFF.PROC1, on and off again), so it waits in the
/// boot ROM for `Multicore::spawn`, and free the SIO spinlocks. It isn't
/// left held off: with core 1 forced off the chip's SWD port doesn't answer
/// at all, so a debugger can't attach during the countdown or setup. A reset of
/// core 0 alone (a debugger's, or SYSRESETREQ) can leave core 1 running its
/// I/O loop: using the spinlocks, the code in SRAM that `copy_core1_text`
/// rewrites, and the clocks, pins and QMI this stub sets up again.
pub fn core1_hold() {
    // SAFETY: PSM register, before anything else uses the PSM.
    let psm = unsafe { &*hal::pac::PSM::ptr() };
    psm.frce_off().modify(|_, w| w.proc1().set_bit());
    while !psm.frce_off().read().proc1().bit_is_set() {}
    // SAFETY: core 1 is held in reset, so no spinlock is in use.
    unsafe { hal::sio::spinlock_reset() };
    // out of reset again, into the boot ROM's wait for a launch
    psm.frce_off().modify(|_, w| w.proc1().clear_bit());
}

/// Go to BOOTSEL if a debugger asked for it via `BOOTSEL_MAGIC`.
pub fn bootsel_if_requested() {
    // SAFETY: watchdog scratch register.
    unsafe {
        if read_volatile(WATCHDOG_SCRATCH0 as *const u32) == BOOTSEL_MAGIC {
            write_volatile(WATCHDOG_SCRATCH0 as *mut u32, 0);
            reboot_to_bootsel();
        }
    }
}
