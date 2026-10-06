; Built-in user program: prints three lines, sleeping between them (exercises the scheduler).
bits 32
org 0x800000

start:
	mov esi, 3
.loop:
	mov eax, 2		; SYS_WRITE
	mov ebx, msg
	mov ecx, msg_len
	int 0x80

	mov eax, 6		; SYS_SLEEP 300 ms
	mov ebx, 300
	int 0x80

	dec esi
	jnz .loop

	mov eax, 1		; SYS_EXIT with code 7
	mov ebx, 7
	int 0x80
	jmp $

msg:	db "counter: tick", 10
msg_len equ $ - msg
