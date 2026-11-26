/* From rp-hal's rp235x-hal-examples/memory.x (https://github.com/rp-rs/rp-hal).
 * Changed: RAM is the top 32 KiB of SRAM0-7 only. Linux uses the rest
 * (0x20000000-0x20077FFF): kernel data with CONFIG_RP2350_SRAM_DATA, and
 * the sram-text MTD (rp2350.dts). License text: LICENSE-rp-hal.
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 The rp-rs Developers
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
MEMORY {
    /*
     * The RP2350 has either external or internal flash.
     *
     * 2 MiB is a safe default here, although a Pico 2 has 4 MiB.
     */
    FLASH : ORIGIN = 0x10000000, LENGTH = 1024K
    /*
     * RAM consists of 8 banks, SRAM0-SRAM7, with a striped mapping.
     * This is usually good for performance, as it distributes load on
     * those banks evenly.
     */
    RAM : ORIGIN = 0x2006C000, LENGTH = 80K
    /*
     * RAM banks 8 and 9 use a direct mapping. They can be used to have
     * memory areas dedicated for some specific job, improving predictability
     * of access times.
     * Example: Separate stacks for core0 and core1.
     */
    SRAM8 : ORIGIN = 0x20080000, LENGTH = 4K
    SRAM9 : ORIGIN = 0x20081000, LENGTH = 4K
}

SECTIONS {
    /* ### Boot ROM info
     *
     * Goes after .vector_table, to keep it in the first 4K of flash
     * where the Boot ROM (and picotool) can find it
     */
    .start_block : ALIGN(4)
    {
        __start_block_addr = .;
        KEEP(*(.start_block));
        KEEP(*(.boot_info));
    } > FLASH

} INSERT AFTER .vector_table;

/* move .text to start /after/ the boot info */
_stext = ADDR(.start_block) + SIZEOF(.start_block);

SECTIONS {
    /* ### Picotool 'Binary Info' Entries
     *
     * Picotool looks through this block (as we have pointers to it in our
     * header) to find interesting information.
     */
    .bi_entries : ALIGN(4)
    {
        /* We put this in the header */
        __bi_entries_start = .;
        /* Here are the entries */
        KEEP(*(.bi_entries));
        /* Keep this block a nice round size */
        . = ALIGN(4);
        /* We put this in the header */
        __bi_entries_end = .;
    } > FLASH
} INSERT AFTER .text;

SECTIONS {
    /* ### Boot ROM extra info
     *
     * Goes after everything in our program, so it can contain a signature.
     */
    .end_block : ALIGN(4)
    {
        __end_block_addr = .;
        KEEP(*(.end_block));
        __flash_binary_end = .;
    } > FLASH

} INSERT AFTER .gnu.sgstubs;

PROVIDE(start_to_end = __end_block_addr - __start_block_addr);
PROVIDE(end_to_start = __start_block_addr - __end_block_addr);

/* core 1's hot code in SRAM, or nothing (feature "core1-sram": build.rs
 * writes core1-sram.x from core1.x) */
INCLUDE core1-sram.x



/* Core 0's stack runs from the top of RAM down to the end of .bss, and
 * nothing stops it there: past .bss's end it overwrites the stub's statics
 * (USB_BUS, the console, core 1's stack), which hangs the boot somewhere
 * later with no message. main() itself is about 4.7 KiB; the setup screen
 * (setup::run) is the deepest path, about 8.5 KiB. Keep big things out of main's frame: the radio's
 * driver future (an async fn's future is its largest state from the
 * start) is made on core 1 for that. The boot banner reports how much of
 * the stack was used ("core 0 stack"). .data, .core1_text and .bss share
 * RAM, so a larger core 1 hot path takes from this stack. */
ASSERT(ORIGIN(RAM) + LENGTH(RAM) - __ebss >= 8K,
       "less than 8 KiB of RAM left for core 0's stack: shrink .core1_text or .bss");
