; BIOS boot sector: print banner, load kernel from disk, enter PM, jump to 1 MiB.
; Ends with boot signature 0xAA55.
bits 16
org 0x7C00

KERNEL_SEG     equ 0x1000		; real-mode load segment → phys 0x10000
KERNEL_OFFSET  equ 0x10000
KERNEL_DEST    equ 0x100000		; image base (matches linker.ld)
KERNEL_ENTRY   equ 0x10000C		; _kernel_main (after 12-byte multiboot header)
KERNEL_SECTORS equ 16			; sectors after the boot sector

start:
	xor ax, ax
	mov ds, ax
	mov ss, ax
	mov sp, 0x7C00
	mov [boot_drive], dl

	; --- print "Tiny OS" ---
	mov si, msg
	mov ah, 0x0E
.print:
	lodsb
	test al, al
	jz .load
	int 0x10
	jmp .print

.load:
	; --- load kernel (CHS sector 2+) to 0x1000:0000 ---
	mov ax, KERNEL_SEG
	mov es, ax
	xor bx, bx
	mov ah, 0x02
	mov al, KERNEL_SECTORS
	mov ch, 0
	mov cl, 2
	mov dh, 0
	mov dl, [boot_drive]
	int 0x13
	jc disk_error

	; --- enable A20 (fast Gate A20) ---
	in al, 0x92
	or al, 2
	out 0x92, al

	; --- enter 32-bit protected mode ---
	cli
	lgdt [gdt_desc]
	mov eax, cr0
	or eax, 1
	mov cr0, eax
	jmp CODE_SEL:pm_entry

disk_error:
	mov ah, 0x0E
	mov al, '!'
	int 0x10
	jmp $

bits 32
pm_entry:
	mov ax, DATA_SEL
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax
	mov esp, 0x90000

	; copy kernel low → 1 MiB (linked address)
	mov esi, KERNEL_OFFSET
	mov edi, KERNEL_DEST
	mov ecx, KERNEL_SECTORS * 512 / 4
	rep movsd

	call KERNEL_ENTRY		; _kernel_main
	jmp $

; --- GDT: null, code, data (flat 0..4GiB) ---
gdt:
	dq 0
gdt_code:
	dw 0xFFFF, 0
	db 0, 10011010b, 11001111b, 0
gdt_data:
	dw 0xFFFF, 0
	db 0, 10010010b, 11001111b, 0
gdt_end:

gdt_desc:
	dw gdt_end - gdt - 1
	dd gdt

CODE_SEL equ gdt_code - gdt
DATA_SEL equ gdt_data - gdt

boot_drive: db 0
msg: db "Tiny OS", 0

	times 510 - ($ - $$) db 0
	dw 0xAA55			; boot signature
