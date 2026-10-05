# tinykernel

A tiny 32-bit hobby OS: BIOS bootloader → protected mode → freestanding C kernel with a VGA text console.

## Prerequisites

- `gcc` (MinGW-w64, 32-bit capable)
- `nasm`
- `make`
- `binutils` (`ld`, `objcopy`)
- `qemu-system-i386`
- `dd` (Git Bash / MSYS2)

On Windows with Git Bash, if tools are missing from `PATH`, open a new terminal or run `source ~/.bashrc`.

## Project layout

```
boot/boot.asm       BIOS boot sector (load kernel, enter PM, jump to 1 MiB)
kernel/kernel_main.c
kernel/console.c    VGA putchar / write / clear / printf
kernel/multiboot.c  Multiboot header (for QEMU -kernel)
kernel/linker.ld
include/console.h
Makefile
build/              build outputs (gitignored)
```

## Build

```bash
make
```

Produces `build/os-image.bin` (boot sector + kernel).

## Run

```bash
make run
```

Boots the disk image in QEMU. You should see console output such as:

```
Tiny OS
A
kernel ok boot=1
```

Close the QEMU window to quit.

### Other targets

| Command              | What it does                                              |
| -------------------- | --------------------------------------------------------- |
| `make` / `make all`  | Build `build/os-image.bin`                                |
| `make run`           | Boot via BIOS bootloader in QEMU                          |
| `make run-multiboot` | Load Multiboot ELF with `qemu -kernel` (skips bootloader) |
| `make clean`         | Remove build artifacts                                    |

## Boot flow

1. BIOS loads the first sector to `0x7C00` and jumps there.
2. Bootloader prints a banner, reads the kernel from disk (sector 2+), enables A20, enters 32-bit protected mode.
3. Kernel is copied to `0x100000` and `kernel_main` runs.
4. Console driver writes to the VGA text buffer at `0xB8000`.
