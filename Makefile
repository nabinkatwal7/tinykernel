CC      := gcc
LD      := ld
OBJCOPY := objcopy
QEMU    := qemu-system-i386

CFLAGS  := -m32 -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pic -fno-pie -fno-asynchronous-unwind-tables \
           -Wall -Wextra
# ponytail: MinGW PE may warn "section below image base"; entry/VMA are still 1 MiB.
LDFLAGS := -m i386pe -T kernel/linker.ld -nostdlib

BUILD   := build
KERNEL  := $(BUILD)/kernel.elf
OBJS    := $(BUILD)/multiboot.o $(BUILD)/kernel_main.o

.PHONY: all clean run

all: $(KERNEL)

$(BUILD)/multiboot.o: kernel/multiboot.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel_main.o: kernel/kernel_main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.pe: $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# QEMU -kernel wants ELF + Multiboot; MinGW links PE, then we convert.
$(KERNEL): $(BUILD)/kernel.pe
	$(OBJCOPY) -O elf32-i386 $< $@
	$(OBJCOPY) -O binary -j .multiboot -j .text $(BUILD)/kernel.pe $(BUILD)/kernel.bin

$(BUILD):
	mkdir -p $(BUILD)

# Blank loop: black window, CPU spinning in kernel_main. Ctrl+C / close to quit.
run: $(KERNEL)
	$(QEMU) -kernel $(KERNEL)

clean:
	rm -f $(BUILD)/*.o $(BUILD)/*.pe $(BUILD)/kernel.bin $(BUILD)/kernel.elf
