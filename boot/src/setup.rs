//! The setup screen: a full-screen menu on the USB console, before Linux,
//! where the board config (`config`) is set. It runs on core 0 before core
//! 1 starts, polling the USB console itself, and before the PSRAM is
//! brought up, so it works on a board whose PSRAM pin isn't known yet.
//!
//! It opens when there is no valid config, after `boot::FAILED_BOOTS_MAX`
//! boots in a row that didn't reach Linux's init, when Linux asked for it
//! (`reboot config`), when no PSRAM answers, or on a key during the boot's
//! countdown.
//!
//! Keys: up/down choose a line, left/right change its value, Enter opens a
//! line's list (CPU speed, PSRAM chip select, SD card pins), edits its text
//! (Wi-Fi pins and network, countdown) or runs the action, Esc cancels,
//! Ctrl-L draws the screen again.

use core::fmt::Write as _;

use crate::board::{self, CPU_PRESETS};
use crate::config::{self, Config, CS_AUTO, SD_MISO, WL_CLK, WL_DIO};
use crate::console::{self, Console};
use crate::{boot, flash, io, pins, sd, wifi};
use rp235x_hal as hal;

/// Why setup opened (shown on its status line at first).
#[derive(Clone, Copy)]
pub enum Why {
    Empty,
    /// The sector as tools/wifi.sh left it: a network, no config.
    Legacy,
    Invalid,
    FailedBoots(u32),
    Requested,
    Key,
    NoPsram,
}

pub struct Hw<'a> {
    pub con: &'a mut Console,
    pub resets: &'a mut hal::pac::RESETS,
    pub sys_hz: u32,
    pub peri_hz: u32,
    /// The CPU preset the clocks run at now (a change needs a reboot).
    pub cpu: u8,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Row {
    Cpu,
    Psram,
    Sd,
    Wifi(usize),
    Ssid,
    Pass,
    Countdown,
    FindPsram,
    TestSd,
    TestRadio,
    SaveBoot,
    BootSaved,
    Defaults,
}

const ROWS: [Row; 16] = [
    Row::Cpu,
    Row::Psram,
    Row::Sd,
    Row::Wifi(0),
    Row::Wifi(1),
    Row::Wifi(2),
    Row::Wifi(3),
    Row::Ssid,
    Row::Pass,
    Row::Countdown,
    Row::FindPsram,
    Row::TestSd,
    Row::TestRadio,
    Row::SaveBoot,
    Row::BootSaved,
    Row::Defaults,
];
const WIFI_LABELS: [&str; 4] = ["Wi-Fi WL_ON", "Wi-Fi DIO", "Wi-Fi CS", "Wi-Fi CLK"];
/// The screen line of ROWS[0]; the actions come after a blank line.
const FIRST_LINE: usize = 3;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Key {
    Up,
    Down,
    Left,
    Right,
    Enter,
    Esc,
    Back,
    Redraw,
    Char(u8),
}

/// A line of text on the stack.
struct Line {
    buf: [u8; 200],
    len: usize,
}
impl Line {
    fn new() -> Self {
        Line { buf: [0; 200], len: 0 }
    }
    fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.len]
    }
}
impl core::fmt::Write for Line {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let n = s.len().min(self.buf.len() - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}

/// Text being edited on a line.
struct Edit {
    row: Row,
    buf: [u8; 64],
    len: usize,
}

/// A line's list, open, with the choice under the cursor.
struct Pick {
    row: Row,
    at: usize,
}

struct State {
    cfg: Config,
    saved: Option<Config>,
    sel: usize,
    edit: Option<Edit>,
    pick: Option<Pick>,
    status: Line,
    dtr: bool,
}

fn status(st: &mut State, args: core::fmt::Arguments) {
    st.status = Line::new();
    let _ = st.status.write_fmt(args);
}

fn wait_us(hw: &mut Hw, us: u64) {
    let t0 = io::now_us();
    while io::now_us() - t0 < us {
        hw.con.poll();
    }
}

/// The boot's countdown: true if a key came. The console is polled all
/// along, so the USB port comes up and the terminal sees it count.
#[inline(never)]
pub fn countdown(con: &mut Console, secs: u8) -> bool {
    let t0 = io::now_us();
    let mut shown = u32::MAX;
    loop {
        con.poll();
        if console::ring_getc().is_some() {
            console::ring_drain_input();
            console::ring_print("\n");
            return true;
        }
        let gone = ((io::now_us() - t0) / 1_000_000) as u32;
        if gone >= u32::from(secs) {
            console::ring_print("\r                                                            \r");
            return false;
        }
        let left = u32::from(secs) - gone;
        if left != shown {
            shown = left;
            let mut l = Line::new();
            let _ = write!(l, "\rnativeboot: press any key for setup ({} s) ", left);
            console::ring_write(l.as_bytes());
        }
    }
}

fn read_key(hw: &mut Hw, st: &mut State) -> Key {
    loop {
        hw.con.poll();
        let dtr = console::terminal_open();
        if dtr != st.dtr {
            st.dtr = dtr;
            if dtr {
                return Key::Redraw;
            }
        }
        let Some(b) = console::ring_getc() else { continue };
        return match b {
            0x1b => {
                // an escape sequence, or Esc on its own
                let t0 = io::now_us();
                let mut seq = [0u8; 4];
                let mut n = 0;
                while io::now_us() - t0 < 30_000 && n < seq.len() {
                    hw.con.poll();
                    if let Some(c) = console::ring_getc() {
                        seq[n] = c;
                        n += 1;
                        if n >= 2 && (c.is_ascii_alphabetic() || c == b'~') {
                            break;
                        }
                    }
                }
                match &seq[..n] {
                    [] => Key::Esc,
                    [b'[' | b'O', b'A'] => Key::Up,
                    [b'[' | b'O', b'B'] => Key::Down,
                    [b'[' | b'O', b'C'] => Key::Right,
                    [b'[' | b'O', b'D'] => Key::Left,
                    _ => continue,
                }
            }
            b'\r' | b'\n' => Key::Enter,
            0x7f | 0x08 => Key::Back,
            0x0c => Key::Redraw,
            0x20..=0x7e => Key::Char(b),
            _ => continue,
        };
    }
}

fn gp(l: &mut Line, p: u8) {
    let _ = write!(l, "GP{}", p);
}

fn value(st: &State, row: Row, l: &mut Line) {
    let c = &st.cfg;
    match row {
        Row::Cpu => {
            let p = &CPU_PRESETS[usize::from(c.cpu)];
            let _ = write!(l, "{} MHz at {}.{:02} V", p.mhz, p.mv / 1000, p.mv % 1000 / 10);
            if c.cpu == 0 {
                let _ = write!(l, " (stock)");
            }
        }
        Row::Psram => {
            if c.psram_cs == CS_AUTO {
                let _ = write!(l, "find at boot (GP0, GP8, GP19{})", if pins::count() > 47 { ", GP47" } else { "" });
            } else {
                gp(l, c.psram_cs);
            }
        }
        Row::Sd => {
            let [miso, cs, sck, mosi] = c.sd;
            match sd_group(c) {
                Some(g) => {
                    sd_group_name(l, g);
                    let _ = write!(l, " (MISO {}, CS {}, SCK {}, MOSI {})", miso, cs, sck, mosi);
                }
                None => {
                    let _ = write!(l, "MISO GP{}, CS GP{}, SCK GP{}, MOSI GP{} (custom)", miso, cs, sck, mosi);
                }
            }
        }
        Row::Wifi(i) => {
            gp(l, c.wifi[i]);
            if i == WL_DIO || i == WL_CLK {
                let _ = write!(l, "   (PIO0)");
            }
        }
        Row::Ssid => {
            if c.ssid_len == 0 {
                let _ = write!(l, "(none: no Wi-Fi)");
            } else {
                let _ = l.write_str(core::str::from_utf8(c.ssid()).unwrap_or("?"));
            }
        }
        Row::Pass => {
            if c.pass_len == 0 {
                let _ = write!(l, "(none: an open network)");
            } else {
                for _ in 0..c.pass_len {
                    let _ = l.write_str("*");
                }
            }
        }
        Row::Countdown => {
            let _ = write!(l, "{} s", c.countdown);
            if c.countdown == 0 {
                let _ = write!(l, " (Esc on the terminal can't stop the boot; use 'reboot config')");
            }
        }
        _ => {}
    }
}

// ---- the lists (CPU speed, PSRAM chip select, SD card pins) -----------------

/// How many choices a line's list has (0: the line has no list).
fn choices(row: Row) -> usize {
    match row {
        Row::Cpu => CPU_PRESETS.len(),
        Row::Psram => 1 + pins::CS1_PINS.iter().filter(|&&p| pins::exists(p)).count(),
        // a block of four pins in the SPI order (RX, CSn, SCK, TX), which
        // also picks the SPI: GP0-3 SPI0, GP4-7 SPI0, GP8-11 SPI1, ...
        Row::Sd => usize::from(pins::count() / 4),
        _ => 0,
    }
}

fn psram_choice(i: usize) -> u8 {
    if i == 0 {
        return CS_AUTO;
    }
    pins::CS1_PINS.iter().copied().filter(|&p| pins::exists(p)).nth(i - 1).unwrap_or(CS_AUTO)
}

/// The SD card's block of four pins, if its pins are one.
fn sd_group(c: &Config) -> Option<usize> {
    let base = c.sd[SD_MISO];
    let g = usize::from(base / 4);
    (base.is_multiple_of(4) && c.sd == [base, base + 1, base + 2, base + 3] && g < choices(Row::Sd)).then_some(g)
}

fn sd_group_name(l: &mut Line, g: usize) {
    let base = g * 4;
    let _ = write!(l, "GP{}-{} on SPI{}", base, base + 3, pins::spi_block(base as u8));
}

/// The choice a line's value is now.
fn chosen(c: &Config, row: Row) -> Option<usize> {
    match row {
        Row::Cpu => Some(usize::from(c.cpu)),
        Row::Psram => (0..choices(row)).find(|&i| psram_choice(i) == c.psram_cs),
        Row::Sd => sd_group(c),
        _ => None,
    }
}

fn choose(c: &mut Config, row: Row, i: usize) {
    match row {
        Row::Cpu => c.cpu = i as u8,
        Row::Psram => c.psram_cs = psram_choice(i),
        Row::Sd => {
            let base = (i * 4) as u8;
            c.sd = [base, base + 1, base + 2, base + 3];
        }
        _ => {}
    }
}

/// What else uses pin `p`: the PSRAM, the SD card or the radio (leaving out
/// the line being chosen).
fn pin_user(c: &Config, p: u8, except: Row) -> Option<&'static str> {
    if except != Row::Psram && c.psram_cs == p {
        return Some("PSRAM CS");
    }
    if except != Row::Sd && c.sd.contains(&p) {
        return Some("SD card");
    }
    c.wifi.iter().position(|&q| q == p).map(|i| WIFI_LABELS[i])
}

fn choice_text(c: &Config, row: Row, i: usize, l: &mut Line) {
    match row {
        Row::Cpu => {
            let p = &CPU_PRESETS[i];
            let _ = write!(l, "{} MHz at {}.{:02} V", p.mhz, p.mv / 1000, p.mv % 1000 / 10);
            if i == 0 {
                let _ = write!(l, " (stock)");
            }
        }
        Row::Psram => match psram_choice(i) {
            CS_AUTO => {
                let _ = write!(l, "find at boot");
            }
            p => {
                gp(l, p);
                if let Some(u) = pin_user(c, p, row) {
                    let _ = write!(l, " (in use: {})", u);
                }
            }
        },
        Row::Sd => {
            sd_group_name(l, i);
            let base = (i * 4) as u8;
            if let Some((p, u)) = (base..base + 4).find_map(|p| pin_user(c, p, row).map(|u| (p, u))) {
                let _ = write!(l, " (GP{} in use: {})", p, u);
            }
        }
        _ => {}
    }
}

fn label(row: Row) -> &'static str {
    match row {
        Row::Cpu => "CPU speed",
        Row::Psram => "PSRAM chip select",
        Row::Sd => "SD card pins",
        Row::Wifi(i) => WIFI_LABELS[i],
        Row::Ssid => "Wi-Fi network",
        Row::Pass => "Wi-Fi passphrase",
        Row::Countdown => "Setup countdown",
        Row::FindPsram => "Find the PSRAM",
        Row::TestSd => "Test the SD card",
        Row::TestRadio => "Test the Wi-Fi radio",
        Row::SaveBoot => "Save and boot",
        Row::BootSaved => "Boot without saving",
        Row::Defaults => "Restore the defaults",
    }
}

fn is_action(row: Row) -> bool {
    matches!(row, Row::FindPsram | Row::TestSd | Row::TestRadio | Row::SaveBoot | Row::BootSaved | Row::Defaults)
}

// The terminal's own colours, so the screen reads well in any theme:
// default text, a reverse-video title bar and selection, bold values and
// messages, and black on yellow only for a field being typed into.
const BODY: &[u8] = b"\x1b[0m";
const HEAD: &[u8] = b"\x1b[0;1;7m";
const VALUE: &[u8] = b"\x1b[0;1m";
const SEL: &[u8] = b"\x1b[0;1;7m";
const EDIT: &[u8] = b"\x1b[0;30;43m";
const NOTE: &[u8] = b"\x1b[0;1m";

fn goto(line: usize) {
    goto_at(line, 1);
}

fn goto_at(line: usize, col: usize) {
    let mut l = Line::new();
    let _ = write!(l, "\x1b[{};{}H", line, col);
    console::ring_write(l.as_bytes());
}

/// The screen line of ROWS[i].
fn line_of(i: usize) -> usize {
    FIRST_LINE + i + usize::from(is_action(ROWS[i]))
}

/// The open list, drawn as a box under its line (or above the status line
/// if it would reach it), lined up with the values.
fn draw_pick(st: &State, pk: &Pick) {
    const COL: usize = 22;
    const MAX_W: usize = 80 - COL - 3;
    let n = choices(pk.row);
    let mut w = 0;
    for i in 0..n {
        let mut t = Line::new();
        choice_text(&st.cfg, pk.row, i, &mut t);
        w = w.max(t.len);
    }
    let w = w.min(MAX_W);
    let top = (line_of(st.sel) + 1).min(22 - (n + 2)).max(FIRST_LINE);
    let edge = |line: usize, l: &str, r: &str| {
        goto_at(line, COL);
        console::ring_write(BODY);
        console::ring_write(l.as_bytes());
        for _ in 0..w + 2 {
            console::ring_write("─".as_bytes());
        }
        console::ring_write(r.as_bytes());
    };
    edge(top, "┌", "┐");
    for i in 0..n {
        let mut t = Line::new();
        let _ = t.write_str(" ");
        choice_text(&st.cfg, pk.row, i, &mut t);
        t.len = t.len.min(w + 1);
        while t.len < w + 2 {
            let _ = t.write_str(" ");
        }
        goto_at(top + 1 + i, COL);
        console::ring_write(BODY);
        console::ring_write("│".as_bytes());
        console::ring_write(if i == pk.at { SEL } else { VALUE });
        console::ring_write(t.as_bytes());
        console::ring_write(BODY);
        console::ring_write("│".as_bytes());
    }
    edge(top + 1 + n, "└", "┘");
}

fn draw_row(st: &State, i: usize) {
    let row = ROWS[i];
    goto(line_of(i));
    console::ring_write(BODY);
    console::ring_write(b"\x1b[K");
    let editing = st.edit.as_ref().filter(|e| e.row == row);
    let sel = i == st.sel;
    let mut l = Line::new();
    if is_action(row) {
        let _ = write!(l, "  > {:<76}", label(row));
        console::ring_write(if sel { SEL } else { BODY });
        console::ring_write(l.as_bytes());
        console::ring_write(BODY);
        return;
    }
    // the label, then the value in its own colour
    let _ = write!(l, "  {:<20} ", label(row));
    console::ring_write(if sel { SEL } else { BODY });
    console::ring_write(l.as_bytes());
    let width = 80 - l.len;
    let mut v = Line::new();
    match editing {
        Some(e) => {
            let _ = v.write_str(core::str::from_utf8(&e.buf[..e.len]).unwrap_or(""));
            let _ = v.write_str("_");
        }
        None => value(st, row, &mut v),
    }
    while v.len < width {
        let _ = v.write_str(" ");
    }
    console::ring_write(if editing.is_some() {
        EDIT
    } else if sel {
        SEL
    } else {
        VALUE
    });
    console::ring_write(v.as_bytes());
    console::ring_write(BODY);
}

fn draw_status(st: &State) {
    goto(23);
    console::ring_write(BODY);
    console::ring_write(b"\x1b[K  ");
    console::ring_write(NOTE);
    console::ring_write(st.status.as_bytes());
    console::ring_write(BODY);
}

/// The whole screen. Each line is sent as it is drawn, with the console
/// polled in between, so the 4 KB out ring never overflows.
fn draw(hw: &mut Hw, st: &State) {
    console::ring_write(b"\x1b[?25l");
    console::ring_write(BODY);
    console::ring_write(b"\x1b[2J\x1b[H");
    console::ring_write(HEAD);
    let mut l = Line::new();
    let _ = write!(
        l,
        " RP2350 Setup{:>67}",
        {
            let mut r = Line::new();
            let _ = write!(r, "{}, {} GPIOs, running at {} MHz ", pins::chip_name(), pins::count(), hw.sys_hz / 1_000_000);
            r
        }
        .as_str()
    );
    console::ring_write(l.as_bytes());
    goto(2);
    console::ring_write(BODY);
    console::ring_write(" ──────────────────────────────────────────────────────────────────────────────".as_bytes());
    for i in 0..ROWS.len() {
        draw_row(st, i);
        flush(hw);
    }
    draw_status(st);
    goto(24);
    console::ring_write("\x1b[K  ↑↓ choose   ←→ change   Enter pick, edit or run   Esc cancel   Ctrl-L redraw".as_bytes());
    if let Some(pk) = &st.pick {
        draw_pick(st, pk);
    }
    flush(hw);
}

impl Line {
    fn as_str(&self) -> &str {
        core::str::from_utf8(self.as_bytes()).unwrap_or("")
    }
}
impl core::fmt::Display for Line {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        f.write_str(self.as_str())
    }
}

/// Let the console send what is queued (while a terminal takes it).
fn flush(hw: &mut Hw) {
    let t0 = io::now_us();
    while !console::out_empty() && console::terminal_open() && io::now_us() - t0 < 200_000 {
        hw.con.poll();
    }
}

/// Change the line's value by one step.
fn step(st: &mut State, row: Row, dir: i8) {
    let c = &mut st.cfg;
    let n = choices(row);
    if n > 0 {
        let at = chosen(c, row).map_or(0, |a| (a as isize + dir as isize).rem_euclid(n as isize) as usize);
        choose(c, row, at);
        return;
    }
    match row {
        Row::Wifi(i) => c.wifi[i] = pins::next_pin(c.wifi[i], dir),
        Row::Countdown => {
            let v = i16::from(c.countdown) + i16::from(dir);
            c.countdown = v.clamp(0, i16::from(config::COUNTDOWN_MAX)) as u8;
        }
        _ => {}
    }
}

fn start_edit(st: &mut State, row: Row) {
    let mut e = Edit { row, buf: [0; 64], len: 0 };
    let mut put = |s: &[u8]| {
        let n = s.len().min(64);
        e.buf[..n].copy_from_slice(&s[..n]);
        e.len = n;
    };
    let mut l = Line::new();
    match row {
        Row::Ssid => put(st.cfg.ssid()),
        Row::Pass => put(st.cfg.pass()),
        Row::Wifi(i) => {
            let _ = write!(l, "{}", st.cfg.wifi[i]);
            put(l.as_bytes())
        }
        Row::Countdown => {
            let _ = write!(l, "{}", st.cfg.countdown);
            put(l.as_bytes())
        }
        _ => return,
    }
    st.edit = Some(e);
    status(st, format_args!("Type, then Enter; Esc cancels."));
}

fn finish_edit(st: &mut State) {
    let Some(e) = st.edit.take() else { return };
    let text = &e.buf[..e.len];
    let num = || -> Option<u8> {
        let s = core::str::from_utf8(text).ok()?.trim();
        let s = s.strip_prefix("GP").or_else(|| s.strip_prefix("gp")).unwrap_or(s);
        s.parse::<u8>().ok()
    };
    let c = &mut st.cfg;
    match e.row {
        Row::Ssid => {
            if e.len > 32 {
                status(st, format_args!("A network name is at most 32 bytes."));
                return;
            }
            c.ssid = [0; 32];
            c.ssid[..e.len].copy_from_slice(text);
            c.ssid_len = e.len as u8;
        }
        Row::Pass => {
            if e.len != 0 && !(8..=63).contains(&e.len) {
                status(st, format_args!("A WPA passphrase is 8 to 63 characters (or none)."));
                return;
            }
            c.pass = [0; 64];
            c.pass[..e.len].copy_from_slice(text);
            c.pass_len = e.len as u8;
        }
        Row::Wifi(i) => {
            let Some(p) = num().filter(|&p| pins::exists(p)) else {
                status(st, format_args!("Not a pin of this chip (GP0-GP{}).", pins::count() - 1));
                return;
            };
            c.wifi[i] = p;
        }
        Row::Countdown => match num().filter(|&n| n <= config::COUNTDOWN_MAX) {
            Some(n) => c.countdown = n,
            None => {
                status(st, format_args!("0 to 60 seconds."));
                return;
            }
        },
        _ => {}
    }
    status(st, format_args!(""));
}

/// The SD card's pins on their own (what `Config::check` says about them).
fn sd_pins_ok(c: &Config) -> Result<(), &'static str> {
    let mut only = *c;
    only.wifi = [u8::MAX; 4];
    only.psram_cs = CS_AUTO;
    match only.check() {
        Err("a pin this chip doesn't have") if c.sd.iter().all(|&p| pins::exists(p)) => Ok(()),
        r => r,
    }
}

fn action(hw: &mut Hw, st: &mut State, row: Row) -> Option<Config> {
    match row {
        Row::FindPsram => {
            status(st, format_args!("Looking for the PSRAM..."));
            draw_status(st);
            flush(hw);
            let used = st.cfg.used_pins();
            let found = pins::CS1_PINS
                .iter()
                .copied()
                .filter(|&p| pins::exists(p) && (used.name(p).is_none() || p == st.cfg.psram_cs))
                .find(|&p| board::psram_probe(p, hw.sys_hz).inspect(|_| pins::release(p)).is_some());
            match found {
                Some(p) => {
                    st.cfg.psram_cs = p;
                    status(st, format_args!("PSRAM found: its chip select is GP{}.", p));
                }
                None => status(st, format_args!("No PSRAM answered on GP0, GP8, GP19 or GP47 (pins in use skipped).")),
            }
        }
        Row::TestSd => {
            if let Err(e) = sd_pins_ok(&st.cfg) {
                status(st, format_args!("{}", e));
                return None;
            }
            status(st, format_args!("Talking to the SD card..."));
            draw_status(st);
            flush(hw);
            let p = sd::SdPins(st.cfg.sd);
            let bus = sd::BoardSd::new(p, hw.resets, hw.peri_hz);
            let mut card = sdcard::SdCard::new(bus, sdcard::HIGH_HZ);
            card.start();
            let n = card.sectors();
            sd::BoardSd::release(p);
            if n == 0 {
                status(st, format_args!("No SD card answered on these pins."));
            } else {
                // decimal GB, as cards are sold
                let gb10 = n * 512 / 100_000_000;
                status(st, format_args!("SD card: {}.{} GB.", gb10 / 10, gb10 % 10));
            }
        }
        Row::TestRadio => {
            let [_, dio, _, clk] = st.cfg.wifi;
            if !(dio < 32 && clk < 32 || dio >= 16 && clk >= 16) {
                status(st, format_args!("Wi-Fi DIO and CLK must both be in GP0-31 or in GP16-47."));
                return None;
            }
            status(st, format_args!("Powering the radio up..."));
            draw_status(st);
            flush(hw);
            let v = wifi::probe(hw.resets, hw.sys_hz, wifi::RadioPins(st.cfg.wifi));
            if v == 0xfeed_bead {
                status(st, format_args!("The Wi-Fi radio answers."));
            } else {
                status(st, format_args!("No Wi-Fi radio answered (read {:#010x}, want 0xfeedbead).", v));
            }
        }
        Row::SaveBoot => {
            if let Err(e) = st.cfg.check() {
                status(st, format_args!("Not saved: {}.", e));
                return None;
            }
            let mut img = [0u8; config::LEN];
            st.cfg.encode(&mut img);
            if let Err(e) = flash::write_config(&img) {
                status(st, format_args!("Not saved: the flash write failed ({}).", e));
                return None;
            }
            if Config::load().ok() != Some(st.cfg) {
                status(st, format_args!("Not saved: the flash reads back differently."));
                return None;
            }
            st.saved = Some(st.cfg);
            return Some(st.cfg);
        }
        Row::BootSaved => match st.saved {
            Some(c) => return Some(c),
            None => status(st, format_args!("Nothing is saved yet: save first.")),
        },
        Row::Defaults => {
            let mut d = Config::DEFAULT;
            d.ssid = st.cfg.ssid;
            d.ssid_len = st.cfg.ssid_len;
            d.pass = st.cfg.pass;
            d.pass_len = st.cfg.pass_len;
            st.cfg = d;
            status(st, format_args!("Defaults restored (the network kept); not saved yet."));
        }
        _ => {}
    }
    None
}

/// Run the screen until the user boots. Returns the config to boot with if
/// the clocks already run at its CPU speed; otherwise reboots into it.
#[inline(never)]
pub fn run(hw: &mut Hw, cfg: Config, saved: Option<Config>, why: Why) -> Config {
    boot::set_failed_boots(0);
    let mut st = State { cfg, saved, sel: 0, edit: None, pick: None, status: Line::new(), dtr: console::terminal_open() };
    match why {
        Why::Empty => status(&mut st, format_args!("No board config yet: check each line, then Save and boot.")),
        Why::Legacy => status(&mut st, format_args!("No board config yet; the network is the one wifi.sh stored. Check, then save.")),
        Why::Invalid => status(&mut st, format_args!("The board config in flash isn't valid: check it, then save.")),
        Why::FailedBoots(n) => status(&mut st, format_args!("Linux didn't finish booting {} times in a row: check the settings.", n)),
        Why::Requested => status(&mut st, format_args!("Opened by 'reboot config'.")),
        Why::Key => {}
        Why::NoPsram => status(&mut st, format_args!("No PSRAM answered: Linux needs it. Check its chip select.")),
    }
    draw(hw, &st);
    let chosen = loop {
        let k = read_key(hw, &mut st);
        let row = ROWS[st.sel];
        if let Some(e) = st.edit.as_mut() {
            match k {
                Key::Char(c) if e.len < e.buf.len() => {
                    e.buf[e.len] = c;
                    e.len += 1;
                }
                Key::Back => e.len = e.len.saturating_sub(1),
                Key::Enter => finish_edit(&mut st),
                Key::Esc => {
                    st.edit = None;
                    status(&mut st, format_args!(""));
                }
                Key::Redraw => draw(hw, &st),
                _ => {}
            }
            draw_row(&st, st.sel);
            draw_status(&st);
            continue;
        }
        if let Some(pk) = st.pick.as_mut() {
            let n = choices(pk.row);
            match k {
                Key::Up => pk.at = (pk.at + n - 1) % n,
                Key::Down => pk.at = (pk.at + 1) % n,
                Key::Enter => {
                    let (r, at) = (pk.row, pk.at);
                    choose(&mut st.cfg, r, at);
                    st.pick = None;
                    status(&mut st, format_args!(""));
                    draw(hw, &st);
                    continue;
                }
                Key::Esc => {
                    st.pick = None;
                    status(&mut st, format_args!(""));
                    draw(hw, &st);
                    continue;
                }
                Key::Redraw => {
                    draw(hw, &st);
                    continue;
                }
                _ => continue,
            }
            if let Some(pk) = &st.pick {
                draw_pick(&st, pk);
            }
            continue;
        }
        let before = st.sel;
        match k {
            Key::Up => st.sel = (st.sel + ROWS.len() - 1) % ROWS.len(),
            Key::Down => st.sel = (st.sel + 1) % ROWS.len(),
            Key::Left => step(&mut st, row, -1),
            Key::Right | Key::Char(b' ') => step(&mut st, row, 1),
            Key::Enter if is_action(row) => {
                if let Some(c) = action(hw, &mut st, row) {
                    break c;
                }
                draw(hw, &st);
                continue;
            }
            Key::Enter if choices(row) > 0 => {
                st.pick = Some(Pick { row, at: chosen(&st.cfg, row).unwrap_or(0) });
                status(&mut st, format_args!("Up and down to choose, Enter to take it, Esc to leave it as it was."));
                draw_status(&st);
                if let Some(pk) = &st.pick {
                    draw_pick(&st, pk);
                }
                continue;
            }
            Key::Enter => start_edit(&mut st, row),
            Key::Redraw => {
                draw(hw, &st);
                continue;
            }
            _ => continue,
        }
        draw_row(&st, before);
        draw_row(&st, st.sel);
        draw_status(&st);
    };
    console::ring_write(b"\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
    flush(hw);
    console::ring_drain_input();
    if chosen.cpu != hw.cpu {
        console::ring_print("nativeboot: rebooting at the new CPU speed\n");
        wait_us(hw, 100_000);
        boot::request_next(boot::REQ_NO_COUNTDOWN);
        boot::reboot();
    }
    chosen
}
