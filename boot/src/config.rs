//! The board config: what this board has where (the PSRAM's chip select,
//! the SD card's and the Wi-Fi radio's pins), the CPU speed, the Wi-Fi
//! network, and how long the boot waits for a key. It is set in the setup
//! screen (`setup`), the only place that writes it, and kept in the 4 KB
//! flash sector at `ADDR`. Linux never writes it (flash.rs refuses the
//! sector to Linux); the stub tells Linux what it needs through the device
//! tree it hands over (`fdt`).
//!
//! Layout, little-endian:
//!
//! | Offset | What |
//! |---|---|
//! | 0 | magic "RPCF" |
//! | 4 | version (u16), bytes covered `LEN` (u16) |
//! | 8 | CRC-32 (IEEE) of bytes 12..LEN |
//! | 12 | CPU speed (index into `board::CPU_PRESETS`), countdown (s), PSRAM CS pin (0xff: find it), 0 |
//! | 16 | SD card: MISO, CS, SCK, MOSI |
//! | 20 | Wi-Fi: WL_ON, DIO, CS, CLK |
//! | 24 | SSID length, passphrase length, 0, 0 |
//! | 28 | SSID (32 bytes) |
//! | 60 | passphrase (64 bytes) |
//! | 124 | reserved for options to come, zero (covered by the CRC) |
//! | 1024 | not used |
//!
//! The sector as tools/wifi.sh left it (magic "WIFI", the network only) is
//! not a config, but setup starts from its network.

use crate::board::CPU_PRESETS;
use crate::pins::{self, SpiRole};
use core::ptr::read_volatile;

pub const ADDR: u32 = 0x103F_F000;
const MAGIC: u32 = u32::from_le_bytes(*b"RPCF");
const VERSION: u16 = 1;
/// The bytes the CRC covers: the header, the fields and the reserved space.
/// The rest of the sector stays erased.
pub const LEN: usize = 1024;
const LEGACY_WIFI_MAGIC: u32 = u32::from_le_bytes(*b"WIFI");

/// PSRAM chip select: look for it at boot (`pins::CS1_PINS`).
pub const CS_AUTO: u8 = 0xff;
pub const COUNTDOWN_MAX: u8 = 60;

#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Config {
    pub cpu: u8,
    pub countdown: u8,
    pub psram_cs: u8,
    /// MISO, CS, SCK, MOSI
    pub sd: [u8; 4],
    /// WL_ON, DIO, CS, CLK
    pub wifi: [u8; 4],
    pub ssid: [u8; 32],
    pub ssid_len: u8,
    pub pass: [u8; 64],
    pub pass_len: u8,
}

pub const SD_MISO: usize = 0;
pub const SD_SCK: usize = 2;
pub const WL_DIO: usize = 1;
pub const WL_CLK: usize = 3;

/// Why there is no config in flash.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Missing {
    /// Erased (or never written).
    Empty,
    /// The sector as tools/wifi.sh left it (a network only).
    Legacy,
    /// Something else, or a CRC that doesn't match.
    Invalid,
}

impl Config {
    /// Pico 2 W wiring for the radio, the SD card on SPI1 at GP8-11 and the
    /// PSRAM found at boot; stock clocks.
    pub const DEFAULT: Config = Config {
        cpu: 0,
        countdown: 10,
        psram_cs: CS_AUTO,
        sd: [8, 9, 10, 11],
        wifi: [23, 24, 25, 29],
        ssid: [0; 32],
        ssid_len: 0,
        pass: [0; 64],
        pass_len: 0,
    };

    pub fn ssid(&self) -> &[u8] {
        &self.ssid[..usize::from(self.ssid_len).min(32)]
    }
    pub fn pass(&self) -> &[u8] {
        &self.pass[..usize::from(self.pass_len).min(64)]
    }

    /// The config in flash, or why there is none.
    pub fn load() -> Result<Config, Missing> {
        let b = |i: usize| -> u8 {
            // SAFETY: flash is mapped (XIP) and not written while this runs.
            unsafe { read_volatile((ADDR as usize + i) as *const u8) }
        };
        let w = |i: usize| u32::from_le_bytes([b(i), b(i + 1), b(i + 2), b(i + 3)]);
        if (0..LEN).step_by(4).all(|i| w(i) == u32::MAX) {
            return Err(Missing::Empty);
        }
        let version = u16::from_le_bytes([b(4), b(5)]);
        let len = usize::from(u16::from_le_bytes([b(6), b(7)]));
        if w(0) == LEGACY_WIFI_MAGIC {
            return Err(Missing::Legacy);
        }
        if w(0) != MAGIC || version != VERSION || len != LEN || crc32((12..LEN).map(b)) != w(8) {
            return Err(Missing::Invalid);
        }
        let mut c = Config {
            cpu: b(12),
            countdown: b(13),
            psram_cs: b(14),
            sd: [b(16), b(17), b(18), b(19)],
            wifi: [b(20), b(21), b(22), b(23)],
            ssid: [0; 32],
            ssid_len: b(24),
            pass: [0; 64],
            pass_len: b(25),
        };
        for i in 0..32 {
            c.ssid[i] = b(28 + i);
        }
        for i in 0..64 {
            c.pass[i] = b(60 + i);
        }
        if c.check().is_err() {
            return Err(Missing::Invalid);
        }
        Ok(c)
    }

    /// The defaults, with the network tools/wifi.sh stored, if it did.
    pub fn defaults_from_flash() -> Config {
        let mut c = Config::DEFAULT;
        let w = |i: usize| {
            // SAFETY: as in `load`.
            unsafe { read_volatile((ADDR as usize + 4 * i) as *const u32) }
        };
        let (slen, plen) = (w(1) as usize, w(2) as usize);
        if w(0) == LEGACY_WIFI_MAGIC && (1..=32).contains(&slen) && plen <= 64 {
            for i in 0..slen {
                // SAFETY: as in `load`.
                c.ssid[i] = unsafe { read_volatile((ADDR as usize + 12 + i) as *const u8) };
            }
            for i in 0..plen {
                // SAFETY: as in `load`.
                c.pass[i] = unsafe { read_volatile((ADDR as usize + 44 + i) as *const u8) };
            }
            c.ssid_len = slen as u8;
            c.pass_len = plen as u8;
        }
        c
    }

    /// The first `LEN` bytes of the sector for this config (the rest stays
    /// erased).
    pub fn encode(&self, out: &mut [u8; LEN]) {
        out.fill(0);
        out[0..4].copy_from_slice(&MAGIC.to_le_bytes());
        out[4..6].copy_from_slice(&VERSION.to_le_bytes());
        out[6..8].copy_from_slice(&(LEN as u16).to_le_bytes());
        out[12] = self.cpu;
        out[13] = self.countdown;
        out[14] = self.psram_cs;
        out[16..20].copy_from_slice(&self.sd);
        out[20..24].copy_from_slice(&self.wifi);
        out[24] = self.ssid_len;
        out[25] = self.pass_len;
        out[28..60].copy_from_slice(&self.ssid);
        out[60..124].copy_from_slice(&self.pass);
        let crc = crc32(out[12..LEN].iter().copied());
        out[8..12].copy_from_slice(&crc.to_le_bytes());
    }

    /// Whether this board could run with it; the reason if not.
    pub fn check(&self) -> Result<(), &'static str> {
        if usize::from(self.cpu) >= CPU_PRESETS.len() {
            return Err("unknown CPU speed");
        }
        if self.countdown > COUNTDOWN_MAX {
            return Err("countdown is more than 60 s");
        }
        if self.psram_cs != CS_AUTO && !(pins::CS1_PINS.contains(&self.psram_cs) && pins::exists(self.psram_cs)) {
            return Err("PSRAM CS must be GP0, GP8, GP19 or GP47");
        }
        let used = self.used_pins();
        for (i, &(p, _)) in used.iter().enumerate() {
            if !pins::exists(p) {
                return Err("a pin this chip doesn't have");
            }
            if used[..i].iter().any(|&(q, _)| q == p) {
                return Err("a pin is used twice");
            }
        }
        let [miso, _, sck, mosi] = self.sd;
        if pins::spi_role(miso) != SpiRole::Rx || pins::spi_role(sck) != SpiRole::Sck || pins::spi_role(mosi) != SpiRole::Tx {
            return Err("SD MISO, SCK, MOSI must be SPI RX, SCK, TX pins");
        }
        if pins::spi_block(miso) != pins::spi_block(sck) || pins::spi_block(mosi) != pins::spi_block(sck) {
            return Err("SD MISO, SCK and MOSI must be on the same SPI");
        }
        let (dio, clk) = (self.wifi[WL_DIO], self.wifi[WL_CLK]);
        if !(dio < 32 && clk < 32 || dio >= 16 && clk >= 16) {
            return Err("Wi-Fi DIO and CLK must both be in GP0-31 or in GP16-47");
        }
        if self.ssid_len > 32 || self.pass_len > 64 {
            return Err("network name or passphrase too long");
        }
        if self.pass_len != 0 && !(8..=63).contains(&self.pass_len) {
            return Err("a WPA passphrase is 8 to 63 characters");
        }
        Ok(())
    }

    /// Every pin core 1 keeps, with its name for Linux (gpio-line-names).
    /// The PSRAM's is there unless it is still to be found.
    pub fn used_pins(&self) -> heapless_pins::List {
        let mut l = heapless_pins::List::new();
        if self.psram_cs != CS_AUTO {
            l.push(self.psram_cs, "PSRAM_CS");
        }
        for (p, n) in self.sd.iter().zip(["SD_MISO", "SD_CS", "SD_SCK", "SD_MOSI"]) {
            l.push(*p, n);
        }
        for (p, n) in self.wifi.iter().zip(["WL_ON", "WL_D", "WL_CS", "WL_CLK"]) {
            l.push(*p, n);
        }
        l
    }
}

/// A short fixed list of (pin, name), without an allocator.
pub mod heapless_pins {
    pub struct List {
        items: [(u8, &'static str); 9],
        len: usize,
    }
    impl List {
        pub const fn new() -> Self {
            List { items: [(0, ""); 9], len: 0 }
        }
        pub fn push(&mut self, p: u8, n: &'static str) {
            if self.len < self.items.len() {
                self.items[self.len] = (p, n);
                self.len += 1;
            }
        }
        pub fn name(&self, p: u8) -> Option<&'static str> {
            self.iter().find(|&&(q, _)| q == p).map(|&(_, n)| n)
        }
    }
    impl core::ops::Deref for List {
        type Target = [(u8, &'static str)];
        fn deref(&self) -> &Self::Target {
            &self.items[..self.len]
        }
    }
}

/// CRC-32 (IEEE 802.3, reflected, as zlib's crc32), bit by bit.
pub fn crc32(bytes: impl Iterator<Item = u8>) -> u32 {
    let mut crc = u32::MAX;
    for b in bytes {
        crc ^= u32::from(b);
        for _ in 0..8 {
            crc = if crc & 1 != 0 { (crc >> 1) ^ 0xedb8_8320 } else { crc >> 1 };
        }
    }
    !crc
}
