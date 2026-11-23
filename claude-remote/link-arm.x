/* claude-guest-mcp as an ATRANS v3 program on native RP2350 Linux
 * (atrans): a static executable in two windows.
 *   data window 0x11000000: the stack (128 KiB) below the ELF headers,
 *     .data and .bss (the heap arena)
 *   text window 0x11C00000: code and constants, read-only, shared by every
 *     process running the binary
 * The data segment comes first in the file so that it can hold the headers
 * (AT_PHDR then points into memory the program has, under qemu-user too). */
OUTPUT_ARCH(arm)
ENTRY(_start)
PHDRS {
  data PT_LOAD FILEHDR PHDRS FLAGS(6);
  text PT_LOAD FLAGS(5);
}
SECTIONS {
  . = 0x11020000 + SIZEOF_HEADERS;
  .data : { *(.data .data.*) } :data
  .bss (NOLOAD) : ALIGN(16) { *(.bss .bss.* COMMON) } :data
  . = 0x11C00000;
  .text : { KEEP(*(.text._start)) *(.text .text.*) } :text
  .rodata : { *(.rodata .rodata.*) } :text
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
