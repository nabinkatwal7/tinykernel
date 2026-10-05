/* Link the kernel at 1 MiB — above the low memory BIOS/bootloader use. */
ENTRY(_kernel_main)

SECTIONS
{
	. = 0x100000;

	/* Multiboot header must sit in the first 8 KiB of the image. */
	.multiboot : {
		KEEP(*(.multiboot))
	}

	.text : {
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

	/DISCARD/ : {
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
