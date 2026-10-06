/* The AP trampoline, assembled by nasm into a flat binary and embedded in the kernel. */
.section .rodata
.globl _ap_tramp_start, _ap_tramp_end
_ap_tramp_start:
	.incbin "build/ap_tramp.bin"
_ap_tramp_end:
