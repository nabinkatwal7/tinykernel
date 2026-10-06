/*
 * Kernel entry. The bootloader copies the image to physical 1 MiB and calls 0x10000C with
 * paging OFF, but the image is linked at KERNEL_VMA + 1 MiB. This stub therefore runs "low"
 * (every address it uses is a link-time constant minus KERNEL_VMA), turns on a temporary
 * page directory (boot_pd) that maps the first 64 MiB twice - identity and at KERNEL_VMA -
 * and then jumps into the higher half.
 */
#define KERNEL_VMA 0xC0000000

.section .text.entry,"ax"
.globl _kernel_start
_kernel_start:
	cld
	movl %cr4, %eax
	orl $0x10, %eax                       /* CR4.PSE: 4 MiB pages for the boot mapping */
	movl %eax, %cr4
	movl $(_boot_pd - KERNEL_VMA), %eax
	movl %eax, %cr3
	movl %cr0, %eax
	orl $0x80000000, %eax
	movl %eax, %cr0
	movl $high_half, %eax
	jmp *%eax

high_half:
	/* .bss is not stored in the image (it would be hundreds of sectors of zeros): clear it here. */
	movl $_bss_start, %edi
	movl $_kernel_end, %ecx
	subl %edi, %ecx
	xorl %eax, %eax
	rep stosb
	call _kernel_main
1:	cli
	hlt
	jmp 1b

/* Page directory: entries 0-15 and 768-783 map physical 0-64 MiB (present, writable, 4 MiB). */
.section .bootpd,"aw"
.align 4096
.globl _boot_pd
_boot_pd:
.set i, 0
.rept 16
	.long (i << 22) | 0x83
.set i, i + 1
.endr
.fill 768 - 16, 4, 0
.set i, 0
.rept 16
	.long (i << 22) | 0x83
.set i, i + 1
.endr
.fill 1024 - 768 - 16, 4, 0
