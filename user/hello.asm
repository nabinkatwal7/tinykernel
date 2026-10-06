; Built-in user program: prints a greeting through int 0x80 and exits.
bits 32
org 0x800000

start:
	mov eax, 2		; SYS_WRITE
	mov ebx, msg
	mov ecx, msg_len
	int 0x80

	mov eax, 4		; SYS_TICKS (result in eax, ignored)
	int 0x80

	mov eax, 1		; SYS_EXIT
	xor ebx, ebx
	int 0x80
	jmp $

msg:	db "Hello from user mode (ring 3) via int 0x80!", 10
msg_len equ $ - msg
