; ELF user program (data in its own segment area, entry _start): proves the ELF loader.
bits 32
section .text
global _start
_start:
	mov eax, 2		; SYS_WRITE
	mov ebx, msg
	mov ecx, msg_len
	int 0x80
	mov eax, 1		; SYS_EXIT with the value stored in .data
	mov ebx, [code]
	int 0x80
	jmp $

section .data
msg:	db "Hello from an ELF program (loaded via PT_LOAD segments)!", 10
msg_len equ $ - msg
code:	dd 5
