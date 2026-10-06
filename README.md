# tinykernel

A 32-bit hobby OS for x86, written from scratch: BIOS bootloader with a boot menu → protected mode → higher-half C kernel with paging, preemptive multitasking, a hierarchical filesystem behind a VFS, ring 3 user programs (flat binaries and ELF, with a small C runtime), a TCP/IP stack on an RTL8139 NIC, a PS/2 mouse, a VGA graphics mode with a toy window manager, and a shell to drive all of it.

![demo](docs/demo-screen.png)

## What's inside

- **Boot:** BIOS boot sector with a 4-entry menu (normal / safe / debug / gfx), E820 memory map, multi-track INT 13h kernel load, kernel command line (`/boot.cfg` too), GDT/TSS/IDT, PIC and 100 Hz PIT. The kernel is linked in the higher half (`0xC0100000`).
- **Memory:** bitmap frame allocator, paging with per-task page directories and guard pages, a coalescing `kmalloc` with canaries, a slab allocator, per-task memory accounting.
- **Tasks:** control blocks, round-robin with priorities and aging, sleep queue, wait queues, mutexes, semaphores, IRQ-safe spinlocks, kernel timers, zombies and `task_wait`, a fork experiment, Ctrl+C.
- **Filesystems:** TinyFS v3 (directories, free-space bitmap, `fsck`), an LRU write-back block cache, a VFS with mounts: TinyFS on `/`, read-only FAT12 on `/fat`, `/dev` (null, zero, random, console), `/proc` (version, uptime, meminfo, tasks, mounts, dmesg, cmdline, time).
- **User mode:** ELF and flat loaders, private address spaces, ~25 syscalls (files, descriptors, `exec`, `spawn`, `sbrk`, `lseek`, ...), argv/envp, a C runtime with `string.h`, `stdio.h`, `stdlib.h`, and `ush`, a shell that runs in ring 3.
- **Hardware:** PS/2 keyboard and mouse, serial console (input and output), ATA PIO (two drives), PCI enumeration, CPUID, RTC, PC speaker, ACPI power-off, VGA mode 13h.
- **Network:** RTL8139 driver, ARP, IPv4, ICMP (`ping`), UDP, DHCP, and a small TCP client.
- **Graphics:** BIOS-free mode switching, lines/shapes/text, a graphical console, a window manager with draggable windows and push buttons.
- **Shell:** ~100 commands, history, quoting, `$VAR` expansion, `selftest`.

See [docs/MODULES.md](docs/MODULES.md) for the module map and [docs/DEVELOPER.md](docs/DEVELOPER.md) for the build/test workflow. A recorded run of the `demo` command is in [docs/demo-run.txt](docs/demo-run.txt).

## Prerequisites

- `gcc` (MinGW-w64, 32-bit capable), `nasm`, `make`, `binutils` (`ld`, `objcopy`)
- `qemu-system-i386`
- `dd` (Git Bash / MSYS2)
- Python 3 (only for `tools/qemu_drive.py` and the CI script)

On Windows with Git Bash, if tools are missing from `PATH`, open a new terminal or run `source ~/.bashrc`.

## Build and run

```bash
make          # kernel image, user programs, FAT12 sample image
make run      # QEMU window: floppy = OS, IDE master = build/disk.img, IDE slave = build/fat.img, RTL8139 NIC
```

At the `tiny>` prompt try `help`, then `format` once to create a filesystem on the data disk, then `demo` or `selftest`.

| Command              | What it does                                                          |
| -------------------- | --------------------------------------------------------------------- |
| `make`               | Build `build/os-image.bin` (and `build/fat.img`)                      |
| `make run`           | Boot in QEMU with a data disk; serial log in `build/serial.log`       |
| `make run-headless`  | Same without a window                                                 |
| `make check`         | Clean build, boot smoke test and every self-test (`tools/ci.sh`)      |
| `make clean-disk`    | Delete the data disk (all files)                                      |
| `make clean`         | Remove build artifacts                                                |

`tools/ci.sh --quick` builds and boots; `tools/ci.sh --watch` repeats that whenever a source file changes.

## A tour of the shell

| Area | Commands |
| --- | --- |
| Basics | `help clear version echo date time uptime ticks sleep history color demo` |
| Files | `ls cat write append touch rm cp mv mkdir rmdir cd pwd stat df edit format fsck sync mount` |
| Programs | `install run` (try `run hello`, `run primes 50`, `run ush`) |
| Memory | `meminfo memtest heapcheck slabinfo hexdump vmap vunmap vtrans` |
| Tasks | `ps pstree spawn kill nice timer mutextest semtest prodcons priotest waittest forktest` |
| Hardware | `cpuinfo lspci acpi mouse beep gfx gfxmode wmdemo guidemo disk` |
| Network | `ifconfig dhcp ipconfig arp ping udp tcp netsend nettrace` |
| Tests | `selftest [-v]` and the individual `*test` commands |
| Power | `reboot halt shutdown` |

Kernel parameters come from the boot menu choice and `/boot.cfg`: `safe`, `debug`, `gfx`, `nonet`, `nomouse`, `nobeep`.

## Boot flow

1. BIOS loads the boot sector to `0x7C00`. It shows the menu, stores the choice at `0x900`, stores the E820 map at `0x500`, reads the kernel from the floppy (sector 2+) to `0x10000`, enables A20 and enters protected mode.
2. The kernel is copied to physical `0x100000`; `entry.S` turns on a temporary 4 MiB-page mapping and jumps to the higher half, then calls `kernel_main`.
3. `kernel_main` sets up the GDT, IDT, PIC, memory and paging, CPU/ACPI/PCI discovery, timer, scheduler, keyboard/mouse/serial IRQs, the VFS and disks, the network card and background tasks, then hands control to the shell.

## Project layout

```
boot/boot.asm         boot sector with the boot menu
kernel/*.c, *.S       kernel modules (see docs/MODULES.md)
include/*.h           module headers
user/*.asm            flat and ELF assembly programs
user/c/*.c, user/lib  C programs and their runtime (crt0, syscalls, string/stdio/stdlib)
fatroot/              files that go into the sample FAT12 image
tools/                qemu_drive.py (headless driver), ci.sh, mkfat12.c, genprogs.sh, done.sh
docs/                 module docs, developer guide, notes on paging, ELF, FAT12, PCI, VGA, ...
```
