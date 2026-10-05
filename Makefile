CC      := gcc
LD      := ld
OBJCOPY := objcopy
NASM    := nasm
QEMU    := qemu-system-i386

CFLAGS  := -m32 -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pic -fno-pie -fno-asynchronous-unwind-tables \
           -Wall -Wextra -Iinclude
# ponytail: MinGW PE may warn "section below image base"; entry/VMA are still 1 MiB.
LDFLAGS := -m i386pe -T kernel/linker.ld -nostdlib

BUILD          := build
KERNEL_SECTORS := 16
KERNEL_ELF     := $(BUILD)/kernel.elf
KERNEL_BIN     := $(BUILD)/kernel.bin
BOOT_BIN       := $(BUILD)/boot.bin
IMAGE          := $(BUILD)/os-image.bin
OBJS           := $(BUILD)/multiboot.o $(BUILD)/console.o $(BUILD)/kernel_main.o

.PHONY: all clean run run-multiboot

all: $(IMAGE)

$(BUILD)/multiboot.o: kernel/multiboot.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/console.o: kernel/console.c include/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel_main.o: kernel/kernel_main.c include/console.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.pe: $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(KERNEL_ELF): $(BUILD)/kernel.pe
	$(OBJCOPY) -O elf32-i386 $< $@

# Include .bss so cursor state lands in the image (zeros).
$(KERNEL_BIN): $(BUILD)/kernel.pe
	$(OBJCOPY) -O binary -j .mbhdr -j .text -j .rodata -j .data \
		--set-section-flags .bss=alloc,load,contents -j .bss $< $@

$(BOOT_BIN): boot/boot.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

$(IMAGE): $(BOOT_BIN) $(KERNEL_BIN)
	dd if=/dev/zero of=$@ bs=512 count=$$((1 + $(KERNEL_SECTORS))) status=none
	dd if=$(BOOT_BIN) of=$@ bs=512 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none

$(BUILD):
	mkdir -p $(BUILD)

run: $(IMAGE)
	$(QEMU) -drive format=raw,file=$(IMAGE),if=floppy -boot a

run-multiboot: $(KERNEL_ELF)
	$(QEMU) -kernel $(KERNEL_ELF)

clean:
	rm -f $(BUILD)/*.o $(BUILD)/*.pe $(BUILD)/kernel.bin \
	      $(BUILD)/kernel.elf $(BUILD)/boot.bin $(BUILD)/os-image.bin
