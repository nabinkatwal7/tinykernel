; BIOS boot sector: print banner, load kernel from disk, enter PM, jump to 1 MiB.
; Ends with boot signature 0xAA55.
bits 16
org 0x7C00

KERNEL_SEG     equ 0x1000		; real-mode load segment → phys 0x10000
KERNEL_OFFSET  equ 0x10000
KERNEL_DEST    equ 0x100000		; image base (matches linker.ld)
KERNEL_ENTRY   equ 0x10000C		; _kernel_main (after 12-byte multiboot header)
%ifndef KERNEL_SECTORS
KERNEL_SECTORS equ 256			; sectors after the boot sector (Makefile overrides)
%endif
E820_MAGIC     equ 0x30323845		; "E820" marker so the kernel knows the map is valid
FLOPPY_SPT     equ 18			; 1.44 MB floppy geometry

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
	; --- physical memory map via BIOS int 15h/E820 -> 0x500 (magic, count, 24-byte entries) ---
	xor ax, ax
	mov es, ax
	mov dword [0x500], E820_MAGIC
	mov dword [0x504], 0
	mov di, 0x508
	xor ebx, ebx
.e820:
	mov eax, 0xE820
	mov edx, 0x534D4150		; 'SMAP'
	mov ecx, 24
	int 0x15
	jc .e820_done
	cmp eax, 0x534D4150
	jne .e820_done
	inc dword [0x504]
	add di, 24
	cmp dword [0x504], 32
	jae .e820_done
	test ebx, ebx
	jnz .e820
.e820_done:

	; --- load kernel (BIOS int 13h, one CHS sector at a time, crossing tracks/heads) ---
	mov ax, KERNEL_SEG
	mov es, ax
	xor bx, bx
	mov byte [cyl], 0
	mov byte [head], 0
	mov byte [sec], 2		; sector numbers are 1-based; sector 1 is us
	mov di, KERNEL_SECTORS
.rd:
	mov ah, 0x02
	mov al, 1
	mov ch, [cyl]
	mov cl, [sec]
	mov dh, [head]
	mov dl, [boot_drive]
	int 0x13
	jc disk_error
	add bx, 512
	jnz .adv
	mov ax, es			; offset wrapped past 64 KiB: bump segment
	add ax, 0x1000
	mov es, ax
.adv:
	inc byte [sec]
	cmp byte [sec], FLOPPY_SPT + 1
	jb .next
	mov byte [sec], 1
	inc byte [head]
	cmp byte [head], 2
	jb .next
	mov byte [head], 0
	inc byte [cyl]
.next:
	dec di
	jnz .rd

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
cyl: db 0
head: db 0
sec: db 0
msg: db "Tiny OS", 0

	times 510 - ($ - $$) db 0
	dw 0xAA55			; boot signature
