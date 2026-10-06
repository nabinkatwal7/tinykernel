CC      := gcc
LD      := ld
OBJCOPY := objcopy
NASM    := nasm
QEMU    := qemu-system-i386

CFLAGS  := -m32 -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pic -fno-pie -fno-asynchronous-unwind-tables -mgeneral-regs-only \
           -Wall -Wextra -Iinclude
# ponytail: MinGW PE may warn "section below image base"; entry/VMA are still 1 MiB.
LDFLAGS := -m i386pe -T kernel/linker.ld -nostdlib

BUILD          := build
KERNEL_SECTORS := 256
KERNEL_ELF     := $(BUILD)/kernel.elf
KERNEL_BIN     := $(BUILD)/kernel.bin
BOOT_BIN       := $(BUILD)/boot.bin
IMAGE          := $(BUILD)/os-image.bin
DISK           := $(BUILD)/disk.img
DISK_SECTORS   := 2048

C_SRCS  := $(wildcard kernel/*.c)
S_SRCS  := $(wildcard kernel/*.S)
OBJS    := $(patsubst kernel/%.c,$(BUILD)/%.o,$(C_SRCS)) \
           $(patsubst kernel/%.S,$(BUILD)/%.o,$(S_SRCS))
HEADERS := $(wildcard include/*.h)
USER_BINS := $(BUILD)/hello.bin $(BUILD)/counter.bin $(BUILD)/fault.bin $(BUILD)/evil.bin $(BUILD)/spin.bin

# Headless run: serial log to build/serial.log, no window.
QEMU_DISKS := -drive format=raw,file=$(IMAGE),if=floppy \
              -drive format=raw,file=$(DISK),if=ide,index=0 -boot a

.PHONY: all clean clean-disk run run-headless

all: $(IMAGE)

$(BUILD)/%.o: kernel/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/builtin.o: $(USER_BINS)

$(BUILD)/%.bin: user/%.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/kernel.pe: $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Include .bss so zero-initialised state lands in the image.
$(KERNEL_BIN): $(BUILD)/kernel.pe
	$(OBJCOPY) -O binary -j .mbhdr -j .text -j .rodata -j .data -j .bootpd \
		--set-section-flags .bss=alloc,load,contents -j .bss $< $@
	@test $$(stat -c%s $@) -le $$(($(KERNEL_SECTORS) * 512)) || \
		{ echo "kernel.bin exceeds $(KERNEL_SECTORS) sectors: raise KERNEL_SECTORS"; rm -f $@; exit 1; }

$(BOOT_BIN): boot/boot.asm Makefile | $(BUILD)
	$(NASM) -f bin -DKERNEL_SECTORS=$(KERNEL_SECTORS) $< -o $@

# Pad to a full 1.44 MB floppy so the BIOS geometry always matches the loader.
$(IMAGE): $(BOOT_BIN) $(KERNEL_BIN)
	dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	dd if=$(BOOT_BIN) of=$@ bs=512 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none

# Data disk for the filesystem. Not rebuilt automatically so your files survive; use 'make clean-disk'.
$(DISK): | $(BUILD)
	dd if=/dev/zero of=$@ bs=512 count=$(DISK_SECTORS) status=none

$(BUILD):
	mkdir -p $(BUILD)

run: $(IMAGE) $(DISK)
	$(QEMU) $(QEMU_DISKS) -serial file:$(BUILD)/serial.log

run-headless: $(IMAGE) $(DISK)
	$(QEMU) $(QEMU_DISKS) -display none -serial file:$(BUILD)/serial.log

clean-disk:
	rm -f $(DISK)

clean:
	rm -f $(BUILD)/*.o $(BUILD)/*.pe $(BUILD)/*.bin $(BUILD)/kernel.elf
