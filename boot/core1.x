/* nativeboot, feature "core1-sram": core 1's hot code linked into SRAM.
 * build.rs appends this to memory.x, which cortex-m-rt's link.x includes
 * before its own SECTIONS, so these patterns claim their input sections
 * before `*(.text .text.*)` does. The section is
 * placed after .data in RAM and loaded from flash after .data's image;
 * main() copies it (copy_core1_text) before anything in it runs.
 *
 * In it: the SD card path (the sdcard crate, sd.rs, and its CRC table), the
 * USB console (usb-device, usbd-serial, the HAL's USB driver, console.rs),
 * core 1's loop, the mem* routines, and the radio service's poll (the
 * cyw43 driver's runner is inlined into it, through `poll_driver`, which
 * is #[inline(always)] for that: 9 KB). The rest of the Wi-Fi
 * driver (joins, bring-up, ~10 KB) stays in flash, and its firmware is
 * read through the uncached alias (wifi.rs, `uncached`). Symbols are v0-mangled
 * (`_RNv...6sdcard...`), hence the length-prefixed names. */
SECTIONS {
  .core1_text : ALIGN(4)
  {
    __score1_text = .;
    *(.text.*6sdcard* .text.*10nativeboot2sd* .text.*10nativeboot10core1_loop*)
    *(.text.*10nativeboot7console* .text.*10usb_device* .text.*11usbd_serial* .text.*10rp235x_hal3usb*)
    *(.text.*10nativeboot4wifi*12RadioService*4poll*)
    *(.text.*3mem6memcpy* .text.*3mem7memmove* .text.*3mem6memset* .text.__aeabi_mem*)
    *(.rodata.*6sdcard* .rodata.*10usb_device* .rodata.*11usbd_serial* .rodata.*10nativeboot7console*)
    . = ALIGN(4);
    __ecore1_text = .;
  } > RAM AT > FLASH
} INSERT AFTER .data;
__sicore1_text = LOADADDR(.core1_text);
