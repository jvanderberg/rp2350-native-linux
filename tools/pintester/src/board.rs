//! The two board functions the tester uses: clocks at the stock 150 MHz and
//! the reboot into USB BOOTSEL.

use rp235x_hal as hal;

pub const XTAL_HZ: u32 = 12_000_000;

/// Bring up the clocks (stock 150 MHz) and return the clock manager.
pub fn init_clocks(
    xosc: hal::pac::XOSC,
    clocks: hal::pac::CLOCKS,
    pll_sys: hal::pac::PLL_SYS,
    pll_usb: hal::pac::PLL_USB,
    resets: &mut hal::pac::RESETS,
    watchdog: &mut hal::Watchdog,
) -> hal::clocks::ClocksManager {
    hal::clocks::init_clocks_and_plls(XTAL_HZ, xosc, clocks, pll_sys, pll_usb, resets, watchdog)
        .unwrap()
}

/// Reboot into the bootrom's USB BOOTSEL mode (picotool / mass storage).
pub fn reboot_to_bootsel() -> ! {
    hal::reboot::reboot(
        hal::reboot::RebootKind::BootSel { picoboot_disabled: false, msd_disabled: false },
        hal::reboot::RebootArch::Riscv,
    )
}
