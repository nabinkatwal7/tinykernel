/* Interrupt entry stubs. MinGW i386 prefixes C symbols with '_'. */
.macro ISR_NOERR n
isr\n:
	pushl $0
	pushl $\n
	jmp isr_common
.endm

.macro ISR_ERR n
isr\n:
	pushl $\n
	jmp isr_common
.endm

.text
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

.globl _isr255
_isr255:
	iret                    /* a spurious APIC interrupt needs no EOI */

.globl _isr128
_isr128:
	pushl $0
	pushl $128
	jmp isr_common

isr_common:
	pusha
	pushl %ds
	pushl %es
	pushl %fs
	pushl %gs
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %fs
	movw %ax, %gs
	cld
	pushl %esp
	call _interrupt_dispatch
	addl $4, %esp
	popl %gs
	popl %fs
	popl %es
	popl %ds
	popa
	addl $8, %esp
	iret

.data
.globl _isr_stub_table
_isr_stub_table:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
	.long isr\n
.endr
