//! Flash erase and program for Linux (the /home MTD driver,
//! linux/drivers/rp2350/rp2350-flash.c).
//!
//! Linux on core 0 runs in place from this flash, and its RAM is the PSRAM
//! on the same QMI. While the flash erases or programs, no XIP access may
//! happen on either chip select. So Linux calls `nb_flash_entry` (its
//! address is in the I/O area at `FL_ENTRY`), which lives in SRAM with
//! everything it calls, and:
//! - masks interrupts and moves to an SRAM stack (`FL_STACK_TOP`): the
//!   caller's stack is in PSRAM;
//! - refuses anything outside `REGIONS` (constants here, not in shared
//!   memory): the /home region, 0x10100000-0x101FFFFF, and a 64 KiB scratch
//!   region at 0x10FF0000 for tests. Never the stub, the kernel, the Wi-Fi
//!   sector or the root filesystem. `FL_START`/`FL_END` show the /home one;
//! - parks core 1 in an SRAM loop (`io::serve_park`, rung awake with
//!   `BELL1_PARK`), giving up after a second;
//! - saves the QMI window settings (the board's flash timing, the PSRAM's
//!   QPI setup on CS1), the QSPI pads and XIP_CTRL, calls the bootrom's
//!   `connect_internal_flash`, `flash_exit_xip` and erase or program, and
//!   restores them all;
//! - invalidates only the XIP cache lines of the range it changed: the rest
//!   of the cache, with Linux's dirty PSRAM lines, stays as it was.
//!
//! One call erases one 4 KB sector (about 45 ms, 400 ms at most) or
//! programs one 256-byte page from `FL_PAGE_BUF` (under a millisecond);
//! Linux is frozen meanwhile. An erase of a sector that is already blank
//! is skipped.
//!
//! `nb_flash_entry(op, addr, 0)`: op 1 erases the sector at `addr`, op 2
//! programs the page at `addr` (XIP addresses). Returns 0, -EINVAL (-22)
//! for a bad range, or -EBUSY (-16) if core 1 did not park.

use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::Ordering;

use crate::io::{self, FL_END, FL_PAGE_BUF, FL_START, FL_STACK_TOP, PARK_ACK, PARK_REQ};
use rp235x_hal::rom_data;

pub const REGION_START: u32 = 0x1010_0000;
pub const REGION_END: u32 = 0x1020_0000;
/// The scratch region (the last 64 KiB below 0x11000000), for testing
/// writes without touching /home.
pub const SCRATCH_START: u32 = 0x10ff_0000;
pub const SCRATCH_END: u32 = 0x1100_0000;
const SECTOR: u32 = 4096;
const PAGE: u32 = 256;
const XIP_BASE: u32 = 0x1000_0000;
const XIP_NOCACHE_NOALLOC: u32 = 0x1400_0000;

const OP_ERASE: u32 = 1;
const OP_PROGRAM: u32 = 2;

const QMI: u32 = 0x400d_0000;
const QMI_DIRECT_CSR: u32 = QMI;
const QMI_M0: u32 = QMI + 0x0c;
const QMI_M0_RFMT: u32 = QMI + 0x10;
const QMI_WINDOW_REGS: u32 = 10;
const RFMT_PREFIX_LEN: u32 = 1 << 12;
const DIRECT_EN: u32 = 1 << 0;
const DIRECT_BUSY: u32 = 1 << 1;
const DIRECT_ASSERT_CS0N: u32 = 1 << 2;
const DIRECT_ASSERT_CS1N: u32 = 1 << 3;
const XIP_CTRL: u32 = 0x400c_8000;
const PADS_QSPI_IO: u32 = 0x4004_0004;
const PADS_QSPI_N: u32 = 6;
const XIP_MAINTENANCE: u32 = 0x1800_0000;
const XIP_INVALIDATE_BY_ADDRESS: u32 = 2;
const XIP_LINE: u32 = 8;
const REG_CLR: u32 = 0x3000;
const BLOCK64: u32 = 1 << 16;
const CMD_BLOCK_ERASE: u8 = 0xd8;
const TIMERAWL: u32 = 0x400b_0028;
const PARK_TIMEOUT_US: u32 = 1_000_000;

#[inline(always)]
fn rd(a: u32) -> u32 {
    // SAFETY: a fixed RP2350 register or SRAM address.
    unsafe { read_volatile(a as *const u32) }
}
#[inline(always)]
fn wr(a: u32, v: u32) {
    // SAFETY: as rd.
    unsafe { write_volatile(a as *mut u32, v) }
}

/// Bootrom entry points, looked up at boot (the lookup runs from flash).
#[repr(C)]
struct Rom {
    connect: unsafe extern "C" fn(),
    exit_xip: unsafe extern "C" fn(),
    erase: unsafe extern "C" fn(u32, usize, u32, u8),
    program: unsafe extern "C" fn(u32, *const u8, usize),
}

static mut ROM: Option<Rom> = None;

unsafe extern "C" {
    fn nb_flash_entry(op: u32, addr: u32, len: u32) -> i32;
}

/// Core 0 at boot: look the bootrom functions up and publish the routine.
pub fn init() {
    // SAFETY: core 0 at boot, before core 1 and Linux run.
    unsafe {
        *core::ptr::addr_of_mut!(ROM) = Some(Rom {
            connect: rom_data::connect_internal_flash::ptr(),
            exit_xip: rom_data::flash_exit_xip::ptr(),
            erase: rom_data::flash_range_erase::ptr(),
            program: rom_data::flash_range_program::ptr(),
        });
    }
    io::wr(FL_START, REGION_START);
    io::wr(FL_END, REGION_END);
    io::wr(io::FL_ENTRY, nb_flash_entry as *const () as u32 | 1);
}

// Linux calls this: interrupts off, onto the SRAM stack, the body, back.
core::arch::global_asm!(
    ".section .data.ram_func.flash_entry,\"ax\",%progbits",
    ".balign 4",
    ".global nb_flash_entry",
    ".type nb_flash_entry,%function",
    ".thumb_func",
    "nb_flash_entry:",
    "push {{r4, r5, r6, lr}}",
    "mrs r5, primask",
    "cpsid i",
    "mov r4, sp",
    "ldr r6, ={top}",
    "mov sp, r6",
    "bl nb_flash_body",
    "mov sp, r4",
    "msr primask, r5",
    "pop {{r4, r5, r6, pc}}",
    ".ltorg",
    ".size nb_flash_entry, . - nb_flash_entry",
    top = const FL_STACK_TOP,
);

/// Whether `len` bytes at `addr` read as erased (uncached alias).
#[inline(always)]
fn blank(addr: u32, len: u32) -> bool {
    let mut a = addr - XIP_BASE + XIP_NOCACHE_NOALLOC;
    let end = a + len;
    while a < end {
        if rd(a) != u32::MAX {
            return false;
        }
        a += 4;
    }
    true
}

#[unsafe(no_mangle)]
#[inline(never)]
#[unsafe(link_section = ".data.ram_func.flash_body")]
extern "C" fn nb_flash_body(op: u32, addr: u32, _len: u32) -> i32 {
    let size = if op == OP_ERASE { SECTOR } else { PAGE };
    let inside = |start: u32, end: u32| addr >= start && addr < end && end - addr >= size;
    if (op != OP_ERASE && op != OP_PROGRAM)
        || addr % size != 0
        || !(inside(REGION_START, REGION_END) || inside(SCRATCH_START, SCRATCH_END))
    {
        return -22;
    }
    // SAFETY: set once at boot (`init`), read only.
    let rom = unsafe { &*core::ptr::addr_of!(ROM) };
    let Some(rom) = rom else { return -22 };
    if rd(QMI_M0_RFMT) & (3 * RFMT_PREFIX_LEN) == 0 {
        return -22;
    }
    if op == OP_ERASE && blank(addr, SECTOR) {
        return 0;
    }
    // core 1 into its SRAM loop
    PARK_REQ.store(true, Ordering::SeqCst);
    wr(0xd000_0180, crate::io::BELL1_PARK); // SIO DOORBELL_OUT_SET
    let t0 = rd(TIMERAWL);
    while !PARK_ACK.load(Ordering::SeqCst) {
        if rd(TIMERAWL).wrapping_sub(t0) > PARK_TIMEOUT_US {
            PARK_REQ.store(false, Ordering::SeqCst);
            return -16;
        }
    }
    let t1 = rd(TIMERAWL);
    let offs = addr - XIP_BASE;

    // uninitialised, so no memclr call (that one is in flash)
    let mut qmi = core::mem::MaybeUninit::<[u32; QMI_WINDOW_REGS as usize]>::uninit();
    let mut pads = core::mem::MaybeUninit::<[u32; PADS_QSPI_N as usize]>::uninit();
    let qp = qmi.as_mut_ptr().cast::<u32>();
    let pp = pads.as_mut_ptr().cast::<u32>();
    let mut i = 0;
    // SAFETY: i < the arrays' lengths throughout.
    unsafe {
        while i < QMI_WINDOW_REGS {
            qp.add(i as usize).write_volatile(rd(QMI_M0 + 4 * i));
            i += 1;
        }
        i = 0;
        while i < PADS_QSPI_N {
            pp.add(i as usize).write_volatile(rd(PADS_QSPI_IO + 4 * i));
            i += 1;
        }
    }
    let xip_ctrl = rd(XIP_CTRL);
    // SAFETY: bootrom code (not XIP); interrupts are masked, core 1 is
    // parked and the page is in SRAM.
    unsafe {
        (rom.connect)();
        (rom.exit_xip)();
        if op == OP_ERASE {
            (rom.erase)(offs, SECTOR as usize, BLOCK64, CMD_BLOCK_ERASE);
        } else {
            (rom.program)(offs, FL_PAGE_BUF as *const u8, PAGE as usize);
        }
    }
    // out of direct mode, then the saved windows, pads and XIP_CTRL
    wr(QMI_DIRECT_CSR + REG_CLR, DIRECT_EN | DIRECT_ASSERT_CS0N | DIRECT_ASSERT_CS1N);
    while rd(QMI_DIRECT_CSR) & (DIRECT_EN | DIRECT_BUSY) != 0 {}
    // SAFETY: as above.
    unsafe {
        i = 0;
        while i < QMI_WINDOW_REGS {
            wr(QMI_M0 + 4 * i, qp.add(i as usize).read_volatile());
            i += 1;
        }
        i = 0;
        while i < PADS_QSPI_N {
            wr(PADS_QSPI_IO + 4 * i, pp.add(i as usize).read_volatile());
            i += 1;
        }
    }
    wr(XIP_CTRL, xip_ctrl);
    // drop stale cached copies of the changed range
    let mut a = offs;
    while a < offs + size {
        // SAFETY: XIP cache maintenance space; one write per line.
        unsafe { write_volatile((XIP_MAINTENANCE + a + XIP_INVALIDATE_BY_ADDRESS) as *mut u8, 0) };
        a += XIP_LINE;
    }
    rd(QMI_M0 + 4 * (QMI_WINDOW_REGS - 1));
    rd(XIP_CTRL);
    cortex_m::asm::dsb();
    cortex_m::asm::isb();
    let dt = rd(TIMERAWL).wrapping_sub(t1);

    PARK_REQ.store(false, Ordering::SeqCst);
    while PARK_ACK.load(Ordering::SeqCst) {}

    use crate::io::{FL_ERASES, FL_PAGES, FL_STALL_MAX_US, FL_STALL_TOTAL_MS};
    let count = if op == OP_ERASE { FL_ERASES } else { FL_PAGES } as u32;
    wr(count, rd(count) + 1);
    if dt > rd(FL_STALL_MAX_US as u32) {
        wr(FL_STALL_MAX_US as u32, dt);
    }
    // total stall in ms; the microseconds left over in the spare word after it
    let rest = FL_STALL_TOTAL_MS as u32 + 4;
    let us = rd(rest) + dt;
    wr(FL_STALL_TOTAL_MS as u32, rd(FL_STALL_TOTAL_MS as u32) + us / 1000);
    wr(rest, us % 1000);
    0
}
