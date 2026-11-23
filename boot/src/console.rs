//! Core 1's USB console: CDC-ACM to and from the ring console in SRAM8/9
//! (linux/drivers/rp2350/rp2350-ringcon.c). Output waits while no terminal
//! is open, keeping the newest bytes; input is taken only while the in ring
//! has room; opening the port at 1200 baud and closing it reboots into
//! BOOTSEL.

use core::ptr::{read_volatile, write_volatile};
use core::sync::atomic::{fence, Ordering};

use crate::board;
use rp235x_hal as hal;
use usb_device::class_prelude::UsbBusAllocator;
use usb_device::prelude::*;
use usbd_serial::SerialPort;

use crate::io::{rd, wr};

pub const RING: usize = 0x2008_0000;
pub const MAGIC: usize = RING;
pub const OUT_SIZE_R: usize = RING + 0x04;
pub const IN_SIZE_R: usize = RING + 0x08;
const FLAGS: usize = RING + 0x0c;
pub const OUT_HEAD: usize = RING + 0x10;
pub const OUT_TAIL: usize = RING + 0x14;
const IN_HEAD: usize = RING + 0x18;
const IN_TAIL: usize = RING + 0x1c;
const IN_BUF: usize = RING + 0x100;
pub const OUT_BUF: usize = RING + 0x1000;
pub const IN_SIZE: u32 = 256;
pub const OUT_SIZE: u32 = 4096;
pub const RCON_MAGIC: u32 = 0x4e4f_4352;
/// With no terminal, the out ring keeps at least this much room.
const KEEP_FREE: u32 = 1024;
/// A terminal that takes nothing for this long is treated as gone.
const STALL_US: u32 = 500_000;
const TIMERAWL: usize = 0x400b_0028;

pub fn ring_init() {
    for a in (RING..RING + 0x100).step_by(4) {
        wr(a, 0);
    }
    wr(OUT_SIZE_R, OUT_SIZE);
    wr(IN_SIZE_R, IN_SIZE);
    fence(Ordering::SeqCst);
    wr(MAGIC, RCON_MAGIC);
}

/// Core 0 output before Linux (drops what doesn't fit).
pub fn ring_print(s: &str) {
    for &b in s.as_bytes() {
        if b == b'\n' {
            ring_putc(b'\r');
        }
        ring_putc(b);
    }
}
fn ring_putc(b: u8) {
    let tail = rd(OUT_TAIL);
    if tail.wrapping_sub(rd(OUT_HEAD)) >= OUT_SIZE {
        return;
    }
    // SAFETY: SRAM9, inside the out ring.
    unsafe { write_volatile((OUT_BUF + (tail & (OUT_SIZE - 1)) as usize) as *mut u8, b) };
    fence(Ordering::Release);
    wr(OUT_TAIL, tail.wrapping_add(1));
}

pub struct Console {
    serial: SerialPort<'static, hal::usb::UsbBus>,
    usb: UsbDevice<'static, hal::usb::UsbBus>,
    progress: u32,
}

impl Console {
    pub fn new(usb_bus: &'static UsbBusAllocator<hal::usb::UsbBus>) -> Self {
        let serial = SerialPort::new(usb_bus);
        let usb = UsbDeviceBuilder::new(usb_bus, UsbVidPid(0x2e8a, 0x000a))
            .strings(&[StringDescriptors::default()
                .manufacturer("rp2350-linux")
                .product("native Linux console")
                .serial_number("native")])
            .unwrap()
            .device_class(usbd_serial::USB_CLASS_CDC)
            .build();
        Console { serial, usb, progress: rd(TIMERAWL) }
    }

    /// One pass: USB, input into the in ring, the out ring to the host.
    pub fn poll(&mut self) {
        let now = rd(TIMERAWL);
        let serial = &mut self.serial;
        self.usb.poll(&mut [serial]);
        // input: only while there is room for a whole packet
        let in_tail = rd(IN_TAIL);
        if IN_SIZE - in_tail.wrapping_sub(rd(IN_HEAD)) >= 64 {
            let mut buf = [0u8; 64];
            if let Ok(n) = self.serial.read(&mut buf) {
                for (i, &b) in buf[..n].iter().enumerate() {
                    let at = IN_BUF + (in_tail.wrapping_add(i as u32) & (IN_SIZE - 1)) as usize;
                    // SAFETY: SRAM8, inside the in ring.
                    unsafe { write_volatile(at as *mut u8, b) };
                }
                fence(Ordering::Release);
                wr(IN_TAIL, in_tail.wrapping_add(n as u32));
            }
        }
        let dtr = self.serial.dtr();
        if self.serial.line_coding().data_rate() == 1200 && !dtr {
            board::reboot_to_bootsel();
        }
        wr(FLAGS, u32::from(dtr));
        let mut live = false;
        if dtr {
            loop {
                let head = rd(OUT_HEAD);
                let tail = rd(OUT_TAIL);
                fence(Ordering::Acquire);
                if head == tail {
                    self.progress = now;
                    break;
                }
                // the contiguous part, at most one packet
                let off = (head & (OUT_SIZE - 1)) as usize;
                let n = (tail.wrapping_sub(head) as usize).min(OUT_SIZE as usize - off).min(64);
                let mut buf = [0u8; 64];
                for (i, b) in buf[..n].iter_mut().enumerate() {
                    // SAFETY: SRAM9, inside the out ring.
                    *b = unsafe { read_volatile((OUT_BUF + off + i) as *const u8) };
                }
                match self.serial.write(&buf[..n]) {
                    Ok(k) if k > 0 => {
                        fence(Ordering::Release);
                        wr(OUT_HEAD, head.wrapping_add(k as u32));
                        self.progress = now;
                    }
                    _ => break,
                }
            }
            live = now.wrapping_sub(self.progress) < STALL_US;
        }
        if !live {
            let len = rd(OUT_TAIL).wrapping_sub(rd(OUT_HEAD));
            if len > OUT_SIZE - KEEP_FREE {
                wr(OUT_HEAD, rd(OUT_HEAD).wrapping_add(len - (OUT_SIZE - KEEP_FREE)));
            }
        }
    }
}
