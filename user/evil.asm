; Built-in user program: tries to read kernel memory. Paging must stop it.
bits 32
org 0x800000

start:
	mov eax, [0x100000]	; kernel image: supervisor-only page
	mov eax, 1		; SYS_EXIT (never reached)
	xor ebx, ebx
	int 0x80
	jmp $
