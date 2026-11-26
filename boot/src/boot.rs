//! What one boot tells the next, in POWMAN's scratch registers (they keep
//! their values through the watchdog's chip resets, which is how Linux
//! reboots, and are cleared by a power-on reset; RP2350 datasheet 5.2.3):
//!
//! - SCRATCH0, boots without success: `BOOTS_MAGIC | n`. The stub adds one
//!   before it starts Linux; Linux zeroes it once its init runs
//!   (/proc/rp2350/boot_ok, written by /sbin/overlay-init). Two in a row
//!   that didn't get there open setup instead of a third.
//! - SCRATCH1, a request for the next boot: `REQ_SETUP` (Linux's `reboot
//!   config`, /proc/rp2350/next_boot) opens setup; `REQ_NO_COUNTDOWN`
//!   (setup, after saving) skips the countdown once.
//!
//! linux/mach-rp2350/board.c has the same values.
//!
//! The watchdog: the stub starts it (`watchdog_arm`, about 16.7 s) just
//! before Linux, when the device tree says the kernel feeds it
//! (`/chosen/rp2350,boot-watchdog`). board.c feeds it until boot_ok, then
//! stops it, so a kernel that hangs or panics on the way resets the board
//! and counts as a failed boot.

use core::ptr::{read_volatile, write_volatile};

const POWMAN_SCRATCH0: usize = 0x4010_00b0;
const POWMAN_SCRATCH1: usize = 0x4010_00b4;
const BOOTS_MAGIC: u32 = 0xb007_0000;
const BOOTS_MASK: u32 = 0xffff_0000;
pub const REQ_SETUP: u32 = 0x5345_5455; // "UTES"
pub const REQ_NO_COUNTDOWN: u32 = 0x534b_4950; // "PIKS"
/// Failed boots in a row after which setup opens.
pub const FAILED_BOOTS_MAX: u32 = 2;

const WATCHDOG_CTRL: usize = 0x400d_8000;
const WATCHDOG_LOAD: usize = 0x400d_8004;
const WATCHDOG_ENABLE: u32 = 1 << 30;
const WATCHDOG_TRIGGER: u32 = 1 << 31;
const WATCHDOG_LOAD_MAX: u32 = 0x00ff_ffff;
/// PSM WDSEL: what a watchdog reset resets: every domain but the ring and
/// crystal oscillators (as board.c's restart).
const PSM_WDSEL: usize = 0x4001_8008;
const WDSEL_ALL_BUT_OSC: u32 = 0x01ff_ffff & !(1 << 2 | 1 << 3);
const REG_CLR: usize = 0x3000;

fn rd(a: usize) -> u32 {
    // SAFETY: a POWMAN scratch, watchdog or PSM register.
    unsafe { read_volatile(a as *const u32) }
}
fn wr(a: usize, v: u32) {
    // SAFETY: as rd.
    unsafe { write_volatile(a as *mut u32, v) }
}

/// Boots since the last one that got to Linux's init.
pub fn failed_boots() -> u32 {
    let v = rd(POWMAN_SCRATCH0);
    if v & BOOTS_MASK == BOOTS_MAGIC { v & !BOOTS_MASK } else { 0 }
}

pub fn set_failed_boots(n: u32) {
    wr(POWMAN_SCRATCH0, BOOTS_MAGIC | n.min(0xffff));
}

/// The request for this boot, taken (cleared) as it is read.
pub fn take_request() -> u32 {
    let r = rd(POWMAN_SCRATCH1);
    wr(POWMAN_SCRATCH1, 0);
    r
}

pub fn request_next(r: u32) {
    wr(POWMAN_SCRATCH1, r);
}

/// A reboot (watchdog reset) can leave the watchdog counting: stop it.
pub fn watchdog_off() {
    wr(WATCHDOG_CTRL + REG_CLR, WATCHDOG_ENABLE);
}

/// Start the watchdog at its longest for Linux to feed (board.c).
pub fn watchdog_arm() {
    wr(PSM_WDSEL, WDSEL_ALL_BUT_OSC);
    wr(WATCHDOG_LOAD, WATCHDOG_LOAD_MAX);
    wr(WATCHDOG_CTRL, rd(WATCHDOG_CTRL) | WATCHDOG_ENABLE);
}

/// A whole-chip reset (as board.c's restart).
pub fn reboot() -> ! {
    wr(PSM_WDSEL, WDSEL_ALL_BUT_OSC);
    wr(WATCHDOG_CTRL, WATCHDOG_TRIGGER);
    loop {
        cortex_m::asm::nop();
    }
}
