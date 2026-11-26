//! The device tree Linux gets: the one in flash (`main::DTB`, built by
//! build.sh from rp2350.dts and rp2350-pins.dtsi), copied with this
//! board's config applied to the top `RESERVE` bytes of the PSRAM, which
//! the memory node then leaves out, so Linux never reuses them.
//!
//! What changes (only properties the tree already has, so the strings block
//! is copied as it is):
//! - every `clock-frequency` (clk_sys and clk_peri, which are equal): the
//!   CPU speed;
//! - the memory node's `reg`: the PSRAM's size, at most 8 MiB;
//! - the GPIO controller's `gpio-reserved-ranges` and `gpio-line-names`:
//!   core 1's pins, the fixed pins of the Linux peripherals that stay on,
//!   and on an RP2350A the pins it doesn't have;
//! - on an RP2350A, the chip in the root's `model`;
//! - `status = "disabled"` on a Linux peripheral whose pins (or SPI block)
//!   core 1 has (`LINUX_PERIPHERALS`), and on the ADC of an RP2350A, whose
//!   ADC is on other pins.
//!
//! `/chosen/rp2350,boot-watchdog` says the kernel feeds the watchdog the
//! stub starts (linux/mach-rp2350/board.c); `Patched::watchdog` reports it.

use crate::config::Config;
use crate::pins;
use core::ptr::read_volatile;

const FDT_MAGIC: u32 = 0xd00d_feed;
const FDT_BEGIN_NODE: u32 = 1;
const FDT_END_NODE: u32 = 2;
const FDT_PROP: u32 = 3;
const FDT_NOP: u32 = 4;
const FDT_END: u32 = 9;
const HEADER: usize = 40;

/// PSRAM kept for the tree at the top of the kernel's RAM.
pub const RESERVE: u32 = 16 * 1024;

/// A Linux peripheral on fixed pins (rp2350-pins.dtsi): its node, its
/// pins and their names. `spi0` is also off if the SD card uses SPI0.
struct LinuxPeripheral {
    node: &'static str,
    /// (pin, name, reserved): the chip select of SPI0 is a GPIO Linux's
    /// SPI driver takes through gpiolib, so it isn't reserved
    pins: &'static [(u8, &'static str, bool)],
    spi0: bool,
}

const LINUX_PERIPHERALS: [LinuxPeripheral; 4] = [
    LinuxPeripheral { node: "i2c@40090000", pins: &[(4, "I2C0_SDA", true), (5, "I2C0_SCL", true)], spi0: false },
    LinuxPeripheral { node: "i2c@40098000", pins: &[(6, "I2C1_SDA", true), (7, "I2C1_SCL", true)], spi0: false },
    LinuxPeripheral {
        node: "spi@40080000",
        pins: &[(16, "SPI0_RX", true), (17, "SPI0_CS", false), (18, "SPI0_SCK", true), (19, "SPI0_TX", true)],
        spi0: true,
    },
    LinuxPeripheral {
        node: "adc@400a0000",
        pins: &[
            (40, "ADC0", true),
            (41, "ADC1", true),
            (42, "ADC2", true),
            (43, "ADC3", true),
            (44, "ADC4", true),
            (45, "ADC5", true),
            (46, "ADC6", true),
            (47, "ADC7", true),
        ],
        spi0: false,
    },
];

pub struct Patched {
    /// Where the tree is (the copy, or the one in flash if it couldn't be made).
    pub addr: u32,
    /// The kernel feeds the boot watchdog.
    pub watchdog: bool,
    /// Linux peripherals turned off, as a bit per `LINUX_PERIPHERALS` entry.
    pub disabled: u8,
}

/// What Linux is told about each GPIO.
struct PinPlan {
    reserved: [bool; 48],
    names: [&'static str; 48],
}

const GP_NAMES: [&str; 48] = [
    "GP0", "GP1", "GP2", "GP3", "GP4", "GP5", "GP6", "GP7", "GP8", "GP9", "GP10", "GP11", "GP12", "GP13", "GP14",
    "GP15", "GP16", "GP17", "GP18", "GP19", "GP20", "GP21", "GP22", "GP23", "GP24", "GP25", "GP26", "GP27", "GP28",
    "GP29", "GP30", "GP31", "GP32", "GP33", "GP34", "GP35", "GP36", "GP37", "GP38", "GP39", "GP40", "GP41", "GP42",
    "GP43", "GP44", "GP45", "GP46", "GP47",
];

/// Which Linux peripherals go off, and the pins' reservations and names.
fn plan(c: &Config, psram_cs: u8) -> (u8, PinPlan) {
    let n = pins::count();
    let mut core1 = c.used_pins();
    if c.psram_cs == crate::config::CS_AUTO {
        core1.push(psram_cs, "PSRAM_CS");
    }
    let sd_spi0 = pins::spi_block(c.sd[crate::config::SD_SCK]) == 0;
    let mut disabled = 0u8;
    for (i, p) in LINUX_PERIPHERALS.iter().enumerate() {
        let clash = p.pins.iter().any(|&(q, _, _)| core1.name(q).is_some() || q >= n) || (p.spi0 && sd_spi0);
        if clash {
            disabled |= 1 << i;
        }
    }
    let mut pp = PinPlan { reserved: [false; 48], names: GP_NAMES };
    for (i, p) in LINUX_PERIPHERALS.iter().enumerate() {
        if disabled & 1 << i == 0 {
            for &(q, name, res) in p.pins {
                pp.reserved[q as usize] = res;
                pp.names[q as usize] = name;
            }
        }
    }
    for &(q, name) in core1.iter() {
        pp.reserved[q as usize] = true;
        pp.names[q as usize] = name;
    }
    for q in n..48 {
        pp.reserved[q as usize] = true;
        pp.names[q as usize] = "NC";
    }
    (disabled, pp)
}

/// The big-endian word at `at`, if `b` has it.
fn be(b: &[u8], at: usize) -> Option<u32> {
    let w = sub(b, at, 4)?;
    Some(u32::from_be_bytes([w[0], w[1], w[2], w[3]]))
}

/// `len` bytes of `b` from `at`, if `b` has them.
fn sub(b: &[u8], at: usize, len: usize) -> Option<&[u8]> {
    b.get(at..at.checked_add(len)?)
}

/// Copies the tree at `dtb` with the config applied (see the module docs)
/// to the top `RESERVE` bytes of the kernel's RAM (`ram` bytes at
/// 0x11400000, mapped and writable). Falls back to the tree in flash,
/// unchanged, if it isn't one or the copy would not fit.
pub fn patch(dtb: u32, c: &Config, psram_cs: u8, ram: u32, sys_hz: u32) -> Patched {
    let mem_size = ram - RESERVE;
    let dest = 0x1140_0000 + mem_size;
    let fallback = Patched { addr: dtb, watchdog: false, disabled: 0 };
    // SAFETY: the tree in flash (XIP), read only; its size from its header.
    let rd = |at: usize| unsafe { u32::from_be(read_volatile((dtb as usize + at) as *const u32)) };
    if rd(0) != FDT_MAGIC {
        return fallback;
    }
    let total = rd(4) as usize;
    if !(HEADER..=64 * 1024).contains(&total) {
        return fallback;
    }
    // SAFETY: as above, `total` bytes.
    let src = unsafe { core::slice::from_raw_parts(dtb as *const u8, total) };
    // SAFETY: the top of the kernel's RAM (PSRAM, mapped by ATRANS5/6),
    // which nothing else uses; Linux is told it isn't RAM.
    let out = unsafe { core::slice::from_raw_parts_mut(dest as *mut u8, RESERVE as usize) };
    match build(src, out, c, psram_cs, mem_size, sys_hz) {
        Some((watchdog, disabled)) => Patched { addr: dest, watchdog, disabled },
        None => fallback,
    }
}

/// The rewrite itself: `src` (a whole tree) into `out`. Returns whether
/// /chosen asks for the boot watchdog and what was disabled, or None if
/// `src` isn't a tree this understands or `out` is too small.
pub fn build(src: &[u8], out: &mut [u8], c: &Config, psram_cs: u8, mem_size: u32, sys_hz: u32) -> Option<(bool, u8)> {
    let (disabled, pp) = plan(c, psram_cs);
    // every offset and size is checked: a bad tree is None (the stub then
    // passes the one in flash as it is), never a panic
    if be(src, 0)? != FDT_MAGIC {
        return None;
    }
    let field = |at: usize| be(src, at).map(|v| v as usize);
    let (off_struct, off_strings, off_rsv) = (field(8)?, field(12)?, field(16)?);
    let (size_strings, size_struct) = (field(32)?, field(36)?);
    let header = sub(src, 0, HEADER)?;
    let strings = sub(src, off_strings, size_strings)?;
    sub(src, off_struct, size_struct)?;
    let name_at = |off: usize| -> &[u8] {
        let s = strings.get(off..).unwrap_or(b"");
        &s[..s.iter().position(|&b| b == 0).unwrap_or(s.len())]
    };

    let mut w = Writer { buf: out, len: 0, ok: true };
    w.bytes(header);
    // the memory reservation block, up to and including its terminator
    let rsv_out = w.len;
    let mut r = off_rsv;
    loop {
        let e = sub(src, r, 16)?;
        w.bytes(e);
        r += 16;
        if e.iter().all(|&b| b == 0) {
            break;
        }
    }
    let struct_out = w.len;
    let mut watchdog = false;
    let mut stack: [&[u8]; 8] = [b""; 8];
    let mut depth = 0usize;
    let mut i = off_struct;
    let end = off_struct + size_struct;
    while i < end {
        let tok = be(src, i)?;
        match tok {
            FDT_BEGIN_NODE => {
                let s = src.get(i + 4..end)?;
                let n = s.iter().position(|&b| b == 0)?;
                let len = 4 + (n + 1).next_multiple_of(4);
                w.bytes(sub(src, i, len)?);
                if depth < stack.len() {
                    stack[depth] = &s[..n];
                }
                depth += 1;
                i += len;
            }
            FDT_END_NODE => {
                w.bytes(sub(src, i, 4)?);
                depth = depth.saturating_sub(1);
                i += 4;
            }
            FDT_PROP => {
                let vlen = be(src, i + 4)? as usize;
                let nameoff = be(src, i + 8)?;
                let value = sub(src, i + 12, vlen)?;
                let node = if depth > 0 && depth <= stack.len() { stack[depth - 1] } else { b"" };
                let name = name_at(nameoff as usize);
                if node == b"chosen" && name == b"rp2350,boot-watchdog" {
                    watchdog = true;
                }
                let mut prop = |v: &mut dyn FnMut(&mut Writer)| {
                    w.u32(FDT_PROP);
                    let at = w.len;
                    w.u32(0);
                    w.u32(nameoff);
                    let start = w.len;
                    v(&mut w);
                    let n = w.len - start;
                    w.set_u32(at, n as u32);
                    w.pad4();
                };
                match (node, name) {
                    (_, b"clock-frequency") if vlen == 4 => prop(&mut |w| w.u32(sys_hz)),
                    (b"memory@11400000", b"reg") if vlen == 8 => prop(&mut |w| {
                        w.u32(0x1140_0000);
                        w.u32(mem_size);
                    }),
                    (b"gpio@40028000", b"gpio-reserved-ranges") => prop(&mut |w| {
                        let mut p = 0;
                        while p < 48 {
                            if pp.reserved[p] {
                                let s = p;
                                while p < 48 && pp.reserved[p] {
                                    p += 1;
                                }
                                w.u32(s as u32);
                                w.u32((p - s) as u32);
                            } else {
                                p += 1;
                            }
                        }
                    }),
                    (b"gpio@40028000", b"gpio-line-names") => prop(&mut |w| {
                        for name in pp.names {
                            w.bytes(name.as_bytes());
                            w.bytes(&[0]);
                        }
                    }),
                    // the model names the chip (an RP2350B in the source)
                    (b"", b"model") if pins::count() == 30 && value.starts_with(b"RP2350B") => prop(&mut |w| {
                        w.bytes(b"RP2350A");
                        w.bytes(&value[7..]);
                    }),
                    (n, b"status")
                        if LINUX_PERIPHERALS.iter().enumerate().any(|(k, p)| disabled & 1 << k != 0 && p.node.as_bytes() == n) =>
                    {
                        prop(&mut |w| w.bytes(b"disabled\0"))
                    }
                    _ => prop(&mut |w| w.bytes(value)),
                }
                i += 12 + vlen.next_multiple_of(4);
            }
            FDT_NOP => i += 4,
            FDT_END => {
                w.u32(FDT_END);
                break;
            }
            _ => return None,
        }
    }
    let struct_len = w.len - struct_out;
    let strings_out = w.len;
    w.bytes(strings);
    if !w.ok {
        return None;
    }
    let total_out = w.len;
    w.set_u32(4, total_out as u32);
    w.set_u32(8, struct_out as u32);
    w.set_u32(12, strings_out as u32);
    w.set_u32(16, rsv_out as u32);
    w.set_u32(36, struct_len as u32);
    Some((watchdog, disabled))
}

/// Appends to the output buffer; `ok` drops if anything didn't fit.
struct Writer<'a> {
    buf: &'a mut [u8],
    len: usize,
    ok: bool,
}

impl Writer<'_> {
    fn bytes(&mut self, b: &[u8]) {
        if self.len + b.len() > self.buf.len() {
            self.ok = false;
            return;
        }
        self.buf[self.len..self.len + b.len()].copy_from_slice(b);
        self.len += b.len();
    }
    fn u32(&mut self, v: u32) {
        self.bytes(&v.to_be_bytes());
    }
    fn set_u32(&mut self, at: usize, v: u32) {
        if at + 4 <= self.len {
            self.buf[at..at + 4].copy_from_slice(&v.to_be_bytes());
        }
    }
    fn pad4(&mut self) {
        while !self.len.is_multiple_of(4) {
            self.bytes(&[0]);
        }
    }
}

/// The names of the Linux peripherals turned off, for the boot banner.
pub fn disabled_names(bits: u8) -> impl Iterator<Item = &'static str> {
    const LINUX_NAMES: [&str; 4] = ["i2c0", "i2c1", "spi0", "adc"];
    (0..4).filter(move |i| bits & 1 << i != 0).map(|i| LINUX_NAMES[i])
}
