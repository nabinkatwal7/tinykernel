/* Links user programs for the 0x800000 program window; objcopy then turns the PE into an ELF. */
ENTRY(_start)

SECTIONS
{
	. = 0x800000;

	.text : {
		*(.text .text.*)
	}

	.data : {
		*(.data .data.*)
		*(.rdata*)
		*(.rodata*)
	}

	.bss : {
		*(.bss .bss.*)
		*(COMMON)
	}

	/DISCARD/ : {
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
