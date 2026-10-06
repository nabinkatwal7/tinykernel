; Built-in user program: prints every environment string. Stack: [esp]=argc [esp+4]=argv [esp+8]=envp.
bits 32
org 0x800000

start:
	mov edi, [esp + 8]	; envp
.next:
	mov ebx, [edi]
	test ebx, ebx
	jz .done
	mov ecx, ebx
.len:
	cmp byte [ecx], 0
	je .print
	inc ecx
	jmp .len
.print:
	sub ecx, ebx
	mov eax, 2		; SYS_WRITE
	int 0x80
	mov eax, 3		; SYS_PUTCHAR newline
	mov ebx, 10
	int 0x80
	add edi, 4
	jmp .next
.done:
	mov eax, 1		; SYS_EXIT
	xor ebx, ebx
	int 0x80
	jmp $
