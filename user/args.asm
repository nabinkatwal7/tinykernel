; Built-in user program: prints argc and every argument. Stack at entry: [esp] = argc, [esp+4] = argv.
bits 32
org 0x800000

start:
	mov esi, [esp]		; argc
	mov edi, [esp + 4]	; argv
	xor ebp, ebp		; index
.next:
	cmp ebp, esi
	jae .done

	mov ebx, [edi + ebp*4]	; argv[i]
	mov ecx, ebx
.len:				; ecx = strlen(argv[i]) (via edx)
	cmp byte [ecx], 0
	je .print
	inc ecx
	jmp .len
.print:
	sub ecx, ebx
	mov eax, 2		; SYS_WRITE the argument
	int 0x80
	mov eax, 3		; SYS_PUTCHAR newline
	mov ebx, 10
	int 0x80
	inc ebp
	jmp .next

.done:
	mov eax, 1		; SYS_EXIT with argc as exit code
	mov ebx, esi
	int 0x80
	jmp $
