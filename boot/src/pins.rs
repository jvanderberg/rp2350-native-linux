//! GPIO by number, for the pins the board config (`config`) picks at run
//! time: function select, pads and SIO, as rp235x-hal's gpio module does it
//! for its typed pins (pin.rs, `set_function`); and what each pin may be.
//!
//! The function numbers are the RP2350 datasheet's (Table 646, "GPIO
//! function select"): F1 SPI, F5 SIO, F6 PIO0, F9 QMI CS1n (on GP0, GP8,
//! GP19 and GP47 only); 0x1f is "no function".

use core::ptr::{read_volatile, write_volatile};

const IO_BANK0: usize = 0x4002_8000;
const PADS_BANK0: usize = 0x4003_8000;
const SIO: usize = 0xd000_0000;
const SIO_GPIO_IN: usize = SIO + 0x004;
const SIO_GPIO_HI_IN: usize = SIO + 0x008;
const SIO_OUT_SET: usize = SIO + 0x018;
const SIO_OUT_CLR: usize = SIO + 0x020;
const SIO_OE_SET: usize = SIO + 0x038;
const SIO_OE_CLR: usize = SIO + 0x040;
/// The _HI_ registers (GPIO32-47) follow each low one at +4.
const HI: usize = 4;
/// SYSINFO PACKAGE_SEL: 0 = QFN-80 (RP2350B, GP0-47), 1 = QFN-60 (RP2350A, GP0-29).
const SYSINFO_PACKAGE_SEL: usize = 0x4000_0004;

pub const FUNC_SPI: u32 = 1;
pub const FUNC_SIO: u32 = 5;
pub const FUNC_PIO0: u32 = 6;
pub const FUNC_XIP_CS1: u32 = 9;
pub const FUNC_NULL: u32 = 0x1f;

// pad control bits
const PAD_SLEWFAST: u32 = 1 << 0;
const PAD_SCHMITT: u32 = 1 << 1;
const PAD_PDE: u32 = 1 << 2;
const PAD_PUE: u32 = 1 << 3;
const PAD_DRIVE_LSB: u32 = 4;
const PAD_DRIVE: u32 = 3 << PAD_DRIVE_LSB;
const PAD_IE: u32 = 1 << 6;
const PAD_OD: u32 = 1 << 7;
const PAD_ISO: u32 = 1 << 8;

/// The pins the QMI's second chip select (the PSRAM's) can be on.
pub const CS1_PINS: [u8; 4] = [0, 8, 19, 47];

#[inline(always)]
fn rd(a: usize) -> u32 {
    // SAFETY: an IO_BANK0, PADS_BANK0, SIO or SYSINFO register.
    unsafe { read_volatile(a as *const u32) }
}
#[inline(always)]
fn wr(a: usize, v: u32) {
    // SAFETY: as rd.
    unsafe { write_volatile(a as *mut u32, v) }
}

fn pad(pin: u8) -> usize {
    PADS_BANK0 + 4 + 4 * pin as usize
}

/// How many GPIOs the chip has: 48 (RP2350B) or 30 (RP2350A).
pub fn count() -> u8 {
    if rd(SYSINFO_PACKAGE_SEL) & 1 == 1 { 30 } else { 48 }
}

pub fn chip_name() -> &'static str {
    if count() == 30 { "RP2350A" } else { "RP2350B" }
}

/// Whether the chip has this pin.
pub fn exists(pin: u8) -> bool {
    pin < count()
}

/// Connect `pin` to function `f` (FUNC_*), with its pad as rp235x-hal sets
/// it: input on and output allowed, unless `f` is FUNC_NULL, which isolates it.
pub fn set_function(pin: u8, f: u32) {
    let p = pad(pin);
    let v = rd(p);
    if f == FUNC_NULL {
        wr(p, (v & !PAD_IE) | PAD_OD | PAD_ISO);
    } else {
        wr(p, (v | PAD_IE) & !(PAD_OD | PAD_ISO));
    }
    // FUNCSEL alone; the overrides go back to "as the peripheral says"
    wr(IO_BANK0 + 8 * pin as usize + 4, f);
}

pub fn set_pull(pin: u8, up: bool, down: bool) {
    let p = pad(pin);
    let mut v = rd(p) & !(PAD_PUE | PAD_PDE);
    if up {
        v |= PAD_PUE;
    }
    if down {
        v |= PAD_PDE;
    }
    wr(p, v);
}

/// Drive strength 0..3 (2, 4, 8, 12 mA), fast slew, input Schmitt trigger.
pub fn set_pad(pin: u8, drive: u32, fast: bool, schmitt: bool) {
    let p = pad(pin);
    let mut v = rd(p) & !(PAD_DRIVE | PAD_SLEWFAST | PAD_SCHMITT);
    v |= (drive & 3) << PAD_DRIVE_LSB;
    if fast {
        v |= PAD_SLEWFAST;
    }
    if schmitt {
        v |= PAD_SCHMITT;
    }
    wr(p, v);
}

fn bank(pin: u8) -> (usize, u32) {
    if pin < 32 { (0, 1 << pin) } else { (HI, 1 << (pin - 32)) }
}

/// An SIO output, driven to `high` before its output is enabled.
pub fn output(pin: u8, high: bool) {
    let (o, bit) = bank(pin);
    set(pin, high);
    wr(SIO_OE_SET + o, bit);
    set_function(pin, FUNC_SIO);
}

pub fn set(pin: u8, high: bool) {
    let (o, bit) = bank(pin);
    wr(if high { SIO_OUT_SET } else { SIO_OUT_CLR } + o, bit);
}

pub fn get(pin: u8) -> bool {
    let (o, bit) = bank(pin);
    rd(if o == 0 { SIO_GPIO_IN } else { SIO_GPIO_HI_IN }) & bit != 0
}

/// Back to how reset leaves a pin: no function, pulled down, SIO output off.
pub fn release(pin: u8) {
    let (o, bit) = bank(pin);
    wr(SIO_OE_CLR + o, bit);
    set_function(pin, FUNC_NULL);
    set_pull(pin, false, true);
}

/// An SIO output pin as an embedded-hal `OutputPin` (the radio's power line,
/// the radio's and the SD card's chip selects).
pub struct Out(pub u8);

impl embedded_hal::digital::ErrorType for Out {
    type Error = core::convert::Infallible;
}

impl embedded_hal::digital::OutputPin for Out {
    fn set_low(&mut self) -> Result<(), Self::Error> {
        set(self.0, false);
        Ok(())
    }
    fn set_high(&mut self) -> Result<(), Self::Error> {
        set(self.0, true);
        Ok(())
    }
}

// ---- SPI pins --------------------------------------------------------------------

/// What an SPI function pin is (RX, CSn, SCK, TX repeat every 4 pins).
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum SpiRole {
    Rx = 0,
    Cs = 1,
    Sck = 2,
    Tx = 3,
}

/// Which SPI block (0 or 1) a pin's F1 function belongs to: they alternate
/// every 8 pins (GP0-7 SPI0, GP8-15 SPI1, ...).
pub fn spi_block(pin: u8) -> u8 {
    (pin / 8) % 2
}

pub fn spi_role(pin: u8) -> SpiRole {
    match pin % 4 {
        0 => SpiRole::Rx,
        1 => SpiRole::Cs,
        2 => SpiRole::Sck,
        _ => SpiRole::Tx,
    }
}

/// The next pin the chip has, wrapping.
pub fn next_pin(from: u8, dir: i8) -> u8 {
    (from as i16 + dir as i16).rem_euclid(count() as i16) as u8
}
