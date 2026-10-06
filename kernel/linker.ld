/*
 * Higher-half kernel: virtual 0xC0100000, physical 0x100000 (1 MiB). See entry.S for how the
 * jump from the low-memory bootloader into the higher half works.
 */
ENTRY(_kernel_start)

SECTIONS
{
	. = 0xC0100000;

	/* The bootloader jumps to physical 0x10000C, the first byte after this header. */
	.mbhdr : {
		KEEP(*(.mbhdr))
	}

	.text : {
		*(.text.entry)
		*(.text .text.*)
	}

	.rodata : {
		*(.rodata .rodata.*)
		*(.rdata*)
	}

	.data : {
		*(.data .data.*)
	}

	/* Boot page directory: must be 4 KiB aligned. */
	.bootpd ALIGN(4096) : {
		*(.bootpd)
	}

	.bss : {
		_bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
	}

	_kernel_end = ALIGN(4096);

	/DISCARD/ : {
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
