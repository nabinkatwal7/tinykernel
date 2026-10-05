; BIOS boot sector — print "Tiny OS" via int 0x10, then hang.
bits 16
org 0x7C00

start:
	mov si, msg
	mov ah, 0x0E		; teletype output
.print:
	lodsb
	test al, al
	jz .hang
	int 0x10
	jmp .print

.hang:
	jmp $

msg:	db "Tiny OS", 0

	times 510 - ($ - $$) db 0
	dw 0xAA55
