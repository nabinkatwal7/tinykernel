; Application-processor trampoline. The BSP copies this page to physical 0x8000 and sends a
; STARTUP IPI with vector 0x08: the AP wakes in 16-bit real mode right here, switches to
; protected mode, turns on paging with the kernel's page directory, and calls the C entry point.
bits 16
org 0x8000

start:
	cli
	cld
	xor ax, ax
	mov ds, ax
	lgdt [gdtr]
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	jmp dword 0x08:pm

bits 32
pm:
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	mov eax, [data_cr3]
	mov cr3, eax
	mov eax, cr0
	or eax, 0x80000000
	mov cr0, eax			; paging on: this code is identity mapped, so execution continues
	mov esp, [data_stack]
	lock inc dword [data_started]	; tell the BSP we got this far
	call [data_entry]
.halt:
	cli
	hlt
	jmp .halt

align 8
gdt:
	dq 0
	dq 0x00CF9A000000FFFF		; 0x08: 32-bit code, base 0, limit 4 GiB
	dq 0x00CF92000000FFFF		; 0x10: data
gdtr:
	dw gdtr - gdt - 1
	dd gdt

; Handshake area at a fixed offset the C code knows: 0x8100.
times 0x100 - ($ - $$) db 0
data_cr3:     dd 0
data_stack:   dd 0
data_entry:   dd 0
data_started: dd 0
