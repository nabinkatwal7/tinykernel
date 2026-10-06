/* Link the kernel at 1 MiB — above the low memory BIOS/bootloader use. */
ENTRY(_kernel_start)

SECTIONS
{
	. = 0x100000;

	/* Multiboot header must sit in the first 8 KiB of the image. */
	.mbhdr : {
		KEEP(*(.mbhdr))
	}

	/* The bootloader jumps to the first byte after the header: keep the entry stub first. */
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

	.bss : {
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
