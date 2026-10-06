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
KERNEL_SECTORS := 768
KERNEL_ELF     := $(BUILD)/kernel.elf
KERNEL_BIN     := $(BUILD)/kernel.bin
BOOT_BIN       := $(BUILD)/boot.bin
IMAGE          := $(BUILD)/os-image.bin
DISK           := $(BUILD)/disk.img
DISK_SECTORS   := 2048
FATIMG         := $(BUILD)/fat.img

C_SRCS  := $(wildcard kernel/*.c)
S_SRCS  := $(wildcard kernel/*.S)
OBJS    := $(patsubst kernel/%.c,$(BUILD)/%.o,$(C_SRCS)) \
           $(patsubst kernel/%.S,$(BUILD)/%.o,$(S_SRCS)) $(BUILD)/progs_gen.o
HEADERS := $(wildcard include/*.h)
# User programs are discovered by file name:
#   user/NAME.asm      flat binary (nasm -f bin), linked at 0x800000
#   user/NAME.elf.asm  ELF built from assembly
#   user/c/NAME.c      ELF built from C with the runtime in user/lib
FLAT_PROGS := $(patsubst user/%.asm,%,$(filter-out %.elf.asm,$(wildcard user/*.asm)))
ELF_PROGS  := $(patsubst user/%.elf.asm,%,$(wildcard user/*.elf.asm))
C_PROGS    := $(patsubst user/c/%.c,%,$(wildcard user/c/*.c))
PROG_IMAGES := $(foreach p,$(FLAT_PROGS),$(p):$(BUILD)/$(p).bin) \
               $(foreach p,$(ELF_PROGS),$(p):$(BUILD)/$(p).elf) \
               $(foreach p,$(C_PROGS),$(p):$(BUILD)/c_$(p).elf)
USER_BINS := $(FLAT_PROGS:%=$(BUILD)/%.bin) $(ELF_PROGS:%=$(BUILD)/%.elf) $(C_PROGS:%=$(BUILD)/c_%.elf)

# Headless run: serial log to build/serial.log, no window.
QEMU_NET := -netdev user,id=n0 -device rtl8139,netdev=n0
QEMU_DISKS := $(QEMU_NET) -drive format=raw,file=$(IMAGE),if=floppy \
              -drive format=raw,file=$(DISK),if=ide,index=0 \
              -drive format=raw,file=$(FATIMG),if=ide,index=1 -boot a

.PHONY: all clean clean-disk run run-headless

all: $(IMAGE) $(FATIMG)

$(BUILD)/%.o: kernel/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: kernel/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/progs_gen.c: tools/genprogs.sh Makefile $(USER_BINS) | $(BUILD)
	sh tools/genprogs.sh $@ $(PROG_IMAGES)

$(BUILD)/progs_gen.o: $(BUILD)/progs_gen.c $(HEADERS) $(USER_BINS)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.bin: user/%.asm | $(BUILD)
	$(NASM) -f bin $< -o $@

# ELF user programs: assemble to COFF, link at the user address as PE, convert to ELF.
$(BUILD)/%.elf: user/%.elf.asm user/user.ld | $(BUILD)
	$(NASM) -f win32 $< -o $(BUILD)/$*.uo
	$(LD) -m i386pe -T user/user.ld -nostdlib -o $(BUILD)/$*.upe $(BUILD)/$*.uo
	$(OBJCOPY) -S -O elf32-i386 $(BUILD)/$*.upe $@

# C user programs: user/c/NAME.c -> build/c_NAME.elf, linked with the C runtime in user/lib.
UCFLAGS  := -m32 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie             -fno-asynchronous-unwind-tables -mgeneral-regs-only -Wall -Wextra -Iuser/lib
ULIB_OBJS := $(BUILD)/ucrt0.o $(BUILD)/ulib.o $(BUILD)/ustring.o $(BUILD)/ustdio.o $(BUILD)/umalloc.o

$(BUILD)/ucrt0.o: user/lib/crt0.S | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/ulib.o: user/lib/ulib.c $(wildcard user/lib/*.h) | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/ustring.o: user/lib/ustring.c $(wildcard user/lib/*.h) | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/ustdio.o: user/lib/ustdio.c $(wildcard user/lib/*.h) | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/umalloc.o: user/lib/umalloc.c $(wildcard user/lib/*.h) | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/uc_%.o: user/c/%.c $(wildcard user/lib/*.h) | $(BUILD)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/c_%.elf: $(BUILD)/uc_%.o $(ULIB_OBJS) user/user.ld
	$(LD) -m i386pe -T user/user.ld -nostdlib -o $(BUILD)/c_$*.upe $(ULIB_OBJS) $<
	$(OBJCOPY) -S -O elf32-i386 $(BUILD)/c_$*.upe $@

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

# Host tool + sample FAT12 floppy image (the files under fatroot/), attached as the IDE slave.
$(BUILD)/mkfat12.exe: tools/mkfat12.c | $(BUILD)
	gcc -O1 -o $@ $<

$(FATIMG): $(BUILD)/mkfat12.exe $(wildcard fatroot/* fatroot/*/*)
	$(BUILD)/mkfat12.exe $@ fatroot

run: $(IMAGE) $(DISK) $(FATIMG)
	$(QEMU) $(QEMU_DISKS) -serial file:$(BUILD)/serial.log

run-headless: $(IMAGE) $(DISK) $(FATIMG)
	$(QEMU) $(QEMU_DISKS) -display none -serial file:$(BUILD)/serial.log

clean-disk:
	rm -f $(DISK)

clean:
	rm -f $(BUILD)/*.o $(BUILD)/*.pe $(BUILD)/*.bin $(BUILD)/*.elf $(BUILD)/*.uo $(BUILD)/*.upe
