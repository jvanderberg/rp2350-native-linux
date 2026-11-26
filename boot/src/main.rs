//! Boot stub for native nommu Linux on the RP2350's Cortex-M33 cores.
//!
//! Core 0:
//! 1. the board config (`config`, from flash): it gives the CPU speed;
//!    clocks at that speed (`board`), TIMER0 out of reset with its 1 MHz tick;
//! 2. the console rings in SRAM8/9 (0x20080000), the I/O area (`io`,
//!    0x20068000), the flash routine (`flash`), and the USB console, polled
//!    from core 0 until core 1 takes it over;
//! 3. the setup screen (`setup`) if the config is missing or invalid, after
//!    two boots in a row that didn't reach Linux's init, or if Linux asked
//!    for it (`boot`); otherwise the countdown, where a key opens it;
//! 4. PSRAM on QMI CS1 (setup again if none answers) and the kernel's RAM:
//!    ATRANS5/6 map up to 8 MiB of it at 0x11400000 (ATRANS4, 0x11000000,
//!    stays for user processes);
//! 5. the peripherals core 1 runs (the SD card on SPI0 or SPI1, the Wi-Fi
//!    radio on PIO0, on the config's pins), then core 1;
//! 6. the device tree for Linux (`fdt`: the one in flash at `DTB` with the
//!    config applied), the boot watchdog (`boot`), and a jump to the XIP
//!    kernel at `KERNEL` (Thumb), r0 = 0, r1 = ~0, r2 = that tree.
//!
//! Core 1 is the I/O processor (`core1_loop`): the USB
//! console (`console`), Wi-Fi (`wifi`: frames for Linux's eth0) and the SD
//! card (`sd`), in a loop that sleeps in `wfi` until USB, the radio, a
//! doorbell from Linux or a 1 ms SysTick wakes it. It parks in SRAM while
//! Linux writes flash (`io::serve_park`, `flash`). Its code runs from flash
//! (XIP); its data and stack are in the stub's SRAM (memory.x).
//!
//! The console ring layout is in linux/drivers/rp2350/rp2350-ringcon.c,
//! the I/O area's in `io` and linux/drivers/rp2350/rp2350-io.h.
#![no_std]
#![no_main]

mod board;
mod boot;
mod config;
mod console;
mod fdt;
mod flash;
mod io;
mod pins;
mod sd;
mod setup;
mod wifi;

use core::mem::MaybeUninit;
use core::ptr::{addr_of, addr_of_mut, write_volatile};
use core::sync::atomic::{compiler_fence, fence, Ordering};

use console::ring_print;
use io::{rd, wr};
use panic_rtt_target as _;
use rp235x_hal as hal;
use rtt_target::{rprintln, rtt_init_print};
use usb_device::class_prelude::UsbBusAllocator;

#[unsafe(link_section = ".start_block")]
#[used]
pub static IMAGE_DEF: hal::block::ImageDef = hal::block::ImageDef::secure_exe();

/// Where load.sh puts the kernel (CONFIG_XIP_PHYS_ADDR) and the DTB.
const KERNEL: u32 = 0x10d8_0000;
const DTB: u32 = 0x103f_8000;

const QMI_ATRANS: usize = 0x400d_0034;
const XIP_MAINT: usize = 0x1800_0000;
const TIMERAWL: usize = 0x400b_0028;
/// How long the radio may take to come up before eth0 is reported absent.
const RADIO_BRING_UP_MS: u32 = 10_000;

// ---- core 1: the I/O processor ----------------------------------------------------

static mut USB_BUS: MaybeUninit<UsbBusAllocator<hal::usb::UsbBus>> = MaybeUninit::uninit();
/// The config this boot runs with (the Wi-Fi network is borrowed from it).
static mut CONFIG: config::Config = config::Config::DEFAULT;
/// 24 KiB (6144 words): the radio's driver future lives on it.
static CORE1_STACK: hal::multicore::Stack<6144> = hal::multicore::Stack::new();

const NVIC_ISER0: usize = 0xe000_e100;
const NVIC_ICPR0: usize = 0xe000_e280;
const NVIC_ISPR0: usize = 0xe000_e200;
const ICSR_PENDSTSET: u32 = 1 << 26;
const SYST_CSR: usize = 0xe000_e010;
const SYST_RVR: usize = 0xe000_e014;
const SYST_CVR: usize = 0xe000_e018;
const ICSR: usize = 0xe000_ed04;
const ICSR_PENDSTCLR: u32 = 1 << 25;
const IRQ_USBCTRL: u32 = 14;
const IRQ_PIO0_0: u32 = 15;
const IRQ_SIO_BELL: u32 = 26;

/// Everything core 1 runs, built on core 0. All of it small: core 0's
/// stack is tight (memory.x), so the radio's driver future is made here,
/// on core 1's.
struct Io {
    con: console::Console,
    radio: wifi::Radio,
    sd: sd::SdService,
}
// SAFETY: built on core 0 before core 1 starts, then core 1's alone.
unsafe impl Send for Io {}

fn core1_loop(io: Io, sys_hz: u32) -> ! {
    flash::CORE1_UP.store(true, Ordering::SeqCst);
    let mut con = io.con;
    // the radio's service (and its driver future) stays in this frame
    let mut radio = wifi::RadioService::new(io.radio.run(), RADIO_BRING_UP_MS);
    let mut sd = io.sd;
    // wake sources for wfi; nothing is taken (PRIMASK stays set)
    cortex_m::interrupt::disable();
    // Not the radio's PIO interrupt (the chip's data-ready line): it can
    // stay up while the driver has nothing to do, which would keep core 1 spinning
    // (from flash, through the XIP cache Linux needs). The 1 ms tick bounds
    // the radio's latency instead.
    wr(NVIC_ISER0, 1 << IRQ_USBCTRL | 1 << IRQ_SIO_BELL);
    wr(SYST_RVR, sys_hz / 1000 - 1);
    wr(SYST_CVR, 0);
    wr(SYST_CSR, 0b111); // processor clock, TICKINT, enabled
    loop {
        io::serve_park();
        let pend = rd(NVIC_ISPR0);
        let tick = rd(ICSR) & ICSR_PENDSTSET != 0;
        wr(io::DBG_PASSES, rd(io::DBG_PASSES).wrapping_add(1));
        for (bit, at) in [(IRQ_USBCTRL, io::DBG_WAKE_USB), (IRQ_PIO0_0, io::DBG_WAKE_PIO), (IRQ_SIO_BELL, io::DBG_WAKE_BELL)] {
            if pend & 1 << bit != 0 {
                wr(at, rd(at).wrapping_add(1));
            }
        }
        if tick {
            wr(io::DBG_WAKE_TICK, rd(io::DBG_WAKE_TICK).wrapping_add(1));
        }
        wr(io::DOORBELL_IN_CLR, !0);
        wr(NVIC_ICPR0, !0);
        wr(ICSR, ICSR_PENDSTCLR);
        con.poll();
        radio.poll();
        sd.poll();
        con.poll();
        if !io::PARK_REQ.load(Ordering::Relaxed) {
            cortex_m::asm::wfi();
        }
    }
}

// ---- core 0 -----------------------------------------------------------------------

fn xip_cache_invalidate() {
    for o in (0..16 * 1024).step_by(8) {
        // SAFETY: XIP maintenance window; invalidate by set/way (op 0).
        unsafe { write_volatile((XIP_MAINT + o) as *mut u8, 0) };
    }
    fence(Ordering::SeqCst);
}

/// Hand over to Linux. Never returns.
fn boot_kernel(entry: u32, dtb: u32) -> ! {
    // SAFETY: interrupts off, NVIC quiet, MPU off, then a Thumb jump into the
    // kernel with the boot protocol registers (r0 = 0, r1 = ~0 for DT, r2 = DTB).
    unsafe {
        cortex_m::interrupt::disable();
        for i in 0..2 {
            wr(0xe000_e180 + 4 * i, !0); // NVIC ICER
            wr(0xe000_e280 + 4 * i, !0); // NVIC ICPR
        }
        wr(0xe000_e010, 0); // SysTick off
        wr(0xe000_ed94, 0); // MPU_CTRL off
        // ACTLR.EXTEXCLALL off: the HAL sets it when starting core 1, which
        // sends every LDREX/STREX to the bus. The XIP/PSRAM path has no
        // exclusive monitor, so STREX to kernel RAM would always fail (the
        // kernel spins in its first cmpxchg loop, in printk). Core 0 is the
        // only Linux CPU: the core's local monitor is enough.
        wr(0xe000_e008, rd(0xe000_e008) & !(1 << 29));
        compiler_fence(Ordering::SeqCst);
        cortex_m::asm::dsb();
        cortex_m::asm::isb();
        core::arch::asm!(
            "bx {entry}",
            entry = in(reg) entry | 1,
            in("r0") 0u32,
            in("r1") !0u32,
            in("r2") dtb,
            options(noreturn)
        );
    }
}

/// `say`, then core 0 polls the console for a moment so it gets out
/// before core 1 runs (core 1 polls it from then on).
fn say_now(con: &mut console::Console, args: core::fmt::Arguments) {
    say(args);
    let t0 = rd(TIMERAWL);
    while rd(TIMERAWL).wrapping_sub(t0) < 30_000 {
        con.poll();
    }
}

fn say(args: core::fmt::Arguments) {
    let mut m = Msg::new();
    let _ = core::fmt::write(&mut m, args);
    ring_print(m.as_str());
    rprintln!("{}", m.as_str());
}

/// Copies core 1's SRAM code (core1.x, feature "core1-sram") from flash,
/// word by word with volatile accesses: memcpy itself is among what it
/// copies. Runs first in main, before anything in that code is called.
#[cfg(feature = "core1-sram")]
fn copy_core1_text() {
    unsafe extern "C" {
        static mut __score1_text: u32;
        static mut __ecore1_text: u32;
        static __sicore1_text: u32;
    }
    // SAFETY: the linker's bounds of .core1_text in SRAM and of its image in
    // flash, word aligned; nothing runs from it yet.
    unsafe {
        let mut d = &raw mut __score1_text;
        let mut s = &raw const __sicore1_text;
        while d < &raw mut __ecore1_text {
            d.write_volatile(s.read_volatile());
            d = d.add(1);
            s = s.add(1);
        }
    }
    cortex_m::asm::dsb();
    cortex_m::asm::isb();
}

/// Core 0's stack runs from the top of RAM down to .bss's end, and nothing
/// stops it there (memory.x). `stack_paint` fills what is free with this,
/// and `stack_used` finds how much was ever used, for the boot banner.
const STACK_PAINT: u32 = 0x5717_c0de;

fn stack_bounds() -> (usize, usize) {
    unsafe extern "C" {
        static mut __ebss: u32;
        static mut _stack_start: u32;
    }
    (&raw mut __ebss as usize, &raw mut _stack_start as usize)
}

#[inline(never)]
fn stack_paint() {
    let (lo, _) = stack_bounds();
    let sp = cortex_m::register::msp::read() as usize;
    let mut p = lo.next_multiple_of(4);
    while p + 256 < sp {
        // SAFETY: between .bss's end and well below the stack pointer:
        // free stack nothing uses yet.
        unsafe { write_volatile(p as *mut u32, STACK_PAINT) };
        p += 4;
    }
}

/// The deepest core 0's stack went, and how much there is.
fn stack_used() -> (usize, usize) {
    let (lo, hi) = stack_bounds();
    let mut p = lo.next_multiple_of(4);
    // SAFETY: as in `stack_paint`; reads only.
    while p < hi && unsafe { core::ptr::read_volatile(p as *const u32) } == STACK_PAINT {
        p += 4;
    }
    (hi - p, hi - lo)
}

#[hal::entry]
fn main() -> ! {
    board::core1_hold();
    stack_paint();
    #[cfg(feature = "core1-sram")]
    copy_core1_text();
    board::bootsel_if_requested();
    boot::watchdog_off();
    rtt_init_print!();
    let mut pac = hal::pac::Peripherals::take().unwrap();
    let mut watchdog = hal::Watchdog::new(pac.WATCHDOG);
    // the config first: it has the CPU speed
    let loaded = config::Config::load();
    let mut cfg = loaded.unwrap_or_else(|_| config::Config::defaults_from_flash());
    let cpu = cfg.cpu;
    let preset = &board::CPU_PRESETS[usize::from(cpu)];
    let clocks = board::init_clocks(
        preset,
        pac.XOSC,
        pac.CLOCKS,
        pac.PLL_SYS,
        pac.PLL_USB,
        &mut pac.RESETS,
        &mut watchdog,
    );
    // TIMER0 out of reset (its tick comes from init_clocks' tick generation)
    let _timer = hal::Timer::new_timer0(pac.TIMER0, &mut pac.RESETS, &clocks);
    board::timer_ignore_debug_halt();
    let mut sio = hal::Sio::new(pac.SIO);
    // the GPIO banks out of reset; the pins are used by number from here (`pins`)
    let _ = hal::gpio::Pins::new(pac.IO_BANK0, pac.PADS_BANK0, sio.gpio_bank0, &mut pac.RESETS);
    use hal::Clock as _;
    let sys_hz = clocks.system_clock.freq().to_Hz();
    let peri_hz = clocks.peripheral_clock.freq().to_Hz();

    console::ring_init();
    io::init();
    flash::init();
    // the USB console, polled here until core 1 takes it over
    let usb = hal::usb::UsbBus::new(pac.USB, pac.USB_DPRAM, clocks.usb_clock, true, &mut pac.RESETS);
    // SAFETY: the bus allocator is built once, here, before core 1 runs.
    let usb_bus: &'static UsbBusAllocator<hal::usb::UsbBus> =
        unsafe { (*addr_of_mut!(USB_BUS)).write(UsbBusAllocator::new(usb)) };
    let mut con = console::Console::new(usb_bus);

    say(format_args!(
        "\nnativeboot: {} at {} MHz, {}.{:02} V\n",
        pins::chip_name(),
        sys_hz / 1_000_000,
        preset.mv / 1000,
        preset.mv % 1000 / 10
    ));

    // setup, or the countdown
    let failed = boot::failed_boots();
    let request = boot::take_request();
    let why = match loaded {
        Err(config::Missing::Empty) => Some(setup::Why::Empty),
        Err(config::Missing::Legacy) => Some(setup::Why::Legacy),
        Err(config::Missing::Invalid) => Some(setup::Why::Invalid),
        Ok(_) if failed >= boot::FAILED_BOOTS_MAX => Some(setup::Why::FailedBoots(failed)),
        Ok(_) if request == boot::REQ_SETUP => Some(setup::Why::Requested),
        Ok(_) => None,
    };
    let saved = loaded.ok();
    let mut hw = setup::Hw { con: &mut con, resets: &mut pac.RESETS, sys_hz, peri_hz, cpu };
    match why {
        Some(w) => cfg = setup::run(&mut hw, cfg, saved, w),
        None if request != boot::REQ_NO_COUNTDOWN && cfg.countdown > 0 && setup::countdown(hw.con, cfg.countdown) => {
            cfg = setup::run(&mut hw, cfg, saved, setup::Why::Key);
        }
        None => {}
    }

    // PSRAM first: its setup puts the QMI in direct mode, when no other core
    // may fetch from flash (core 1 starts below)
    let ps = loop {
        let want = if cfg.psram_cs == config::CS_AUTO { None } else { Some(cfg.psram_cs) };
        let used = cfg.used_pins();
        let mut cands = [0u8; 4];
        let mut n = 0;
        for &p in &pins::CS1_PINS {
            if pins::exists(p) && used.name(p).is_none() {
                cands[n] = p;
                n += 1;
            }
        }
        match board::psram_init(want, &cands[..n], sys_hz) {
            Ok(p) => break p,
            Err(e) => {
                say_now(hw.con, format_args!("nativeboot: PSRAM init failed: {:?}\n", e));
                cfg = setup::run(&mut hw, cfg, saved, setup::Why::NoPsram);
            }
        }
    };
    let setup::Hw { con: con_ref, resets, .. } = hw;
    let mem = ps.size.min(8 << 20);
    // kernel RAM: ATRANS5 -> PSRAM 0..4 MiB, ATRANS6 -> 4..8 MiB (SIZE in 4 KiB pages)
    let pages = |b: u32| b >> 12;
    wr(QMI_ATRANS + 4 * 5, pages(mem.min(4 << 20)) << 16);
    if mem > 4 << 20 {
        wr(QMI_ATRANS + 4 * 6, pages(mem - (4 << 20)) << 16 | 0x400);
    }
    xip_cache_invalidate();
    // SAFETY: core 0, before core 1 runs; read only from here on.
    unsafe { *addr_of_mut!(CONFIG) = cfg };
    // SAFETY: as above.
    let cfg: &'static config::Config = unsafe { &*addr_of!(CONFIG) };

    // the SD card, looked for now so a card that is in is there when Linux probes
    let sd = sd::SdService::new(sd::BoardSd::new(sd::SdPins(cfg.sd), resets, peri_hz));
    say_now(con_ref, format_args!(
        "nativeboot: SD card (SPI{}): {}\n",
        pins::spi_block(cfg.sd[config::SD_SCK]),
        if sd.present() { "present" } else { "none" }
    ));

    // the Wi-Fi radio (PIO0); it comes up on core 1 while Linux boots
    let creds = wifi::Creds::from_config(cfg);
    match creds {
        Some(c) => say(format_args!("nativeboot: Wi-Fi: joining {:?} in the background\n", c.ssid())),
        None => say(format_args!("nativeboot: Wi-Fi: no network configured (setup)\n")),
    }
    let radio = wifi::driver(pac.PIO0, resets, sys_hz, wifi::RadioPins(cfg.wifi), creds);

    // the device tree for Linux, before core 1 runs (the copy is in its RAM)
    let dtb_magic = u32::from_be(rd(DTB as usize));
    let k0 = rd(KERNEL as usize);
    if dtb_magic != 0xd00d_feed || k0 == 0xffff_ffff {
        say(format_args!(
            "nativeboot: no kernel/DTB in flash (kernel word {:08x}, DTB magic {:08x}); run load.sh\n",
            k0, dtb_magic
        ));
        loop {
            con.poll();
        }
    }
    let tree = fdt::patch(DTB, cfg, ps.cs, mem, sys_hz);

    let io = Io { con, radio, sd };
    let mut mc = hal::multicore::Multicore::new(&mut pac.PSM, &mut pac.PPB, &mut sio.fifo);
    mc.cores()[1].spawn(CORE1_STACK.take().unwrap(), move || core1_loop(io, sys_hz)).unwrap();

    say(format_args!(
        "nativeboot: PSRAM {} KiB (CS GP{}) at {} MHz, kernel RAM {} KiB at 0x11400000; kernel {:#x}, DTB {:#x}{}\n",
        ps.size >> 10,
        ps.cs,
        ps.sck_hz / 1_000_000,
        (mem - if tree.addr == DTB { 0 } else { fdt::RESERVE }) >> 10,
        KERNEL,
        tree.addr,
        if tree.addr == DTB { " (unpatched)" } else { "" }
    ));
    if tree.disabled != 0 {
        let mut m = Msg::new();
        let _ = core::fmt::write(&mut m, format_args!("nativeboot: core 1 has their pins, so Linux gets no:"));
        for n in fdt::disabled_names(tree.disabled) {
            let _ = core::fmt::write(&mut m, format_args!(" {}", n));
        }
        say(format_args!("{}\n", m.as_str()));
    }
    let (used, size) = stack_used();
    say(format_args!("nativeboot: core 0 stack: {} of {} bytes used\n", used, size));
    say(format_args!("nativeboot: starting Linux\n"));
    // give core 1 a moment to forward the banner if a terminal is open
    let t0 = rd(TIMERAWL);
    while rd(TIMERAWL).wrapping_sub(t0) < 20_000 {}
    boot::set_failed_boots(boot::failed_boots() + 1);
    if tree.watchdog {
        boot::watchdog_arm();
    }
    boot_kernel(KERNEL, tree.addr)
}

/// A small stack-allocated line buffer for formatting.
struct Msg {
    buf: [u8; 192],
    len: usize,
}
impl Msg {
    fn new() -> Self {
        Msg { buf: [0; 192], len: 0 }
    }
    fn as_str(&self) -> &str {
        core::str::from_utf8(&self.buf[..self.len]).unwrap_or("")
    }
}
impl core::fmt::Write for Msg {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let n = s.len().min(self.buf.len() - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&s.as_bytes()[..n]);
        self.len += n;
        Ok(())
    }
}

#[unsafe(link_section = ".bi_entries")]
#[used]
pub static PICOTOOL_ENTRIES: [hal::binary_info::EntryAddr; 2] = [
    hal::binary_info::rp_cargo_bin_name!(),
    hal::binary_info::rp_program_description!(c"Boot stub + I/O core (USB console, Wi-Fi, SD, flash) for native Linux (Cortex-M33)"),
];
