/* First code in the image (linker.ld puts .text.entry first): the bootloader calls 0x10000C. */
.section .text.entry,"ax"
.globl _kernel_start
_kernel_start:
	cld
	call _kernel_main
1:	cli
	hlt
	jmp 1b
