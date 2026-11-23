/*
 * ATRANS v2 programs (docs/atrans.md): the image starts at
 * 0x11020000; the 128 KiB of window below it is the stack. Link higher for
 * a bigger stack.
 */
ENTRY(_start)
SECTIONS
{
	. = 0x11020000;
	__image_start = .;
	.text : { *(.text.start) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	. = ALIGN(8);
	.data : { *(.data .data.*) }
	.bss : { *(.bss .bss.* COMMON) }
	/DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) }
}
