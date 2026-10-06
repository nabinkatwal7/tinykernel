# Kernel modules

Everything lives in `kernel/` (code) and `include/` (one header per module).

| Module | Files | Role |
| --- | --- | --- |
| Boot | `boot/boot.asm` | BIOS boot sector. Reads the E820 memory map to `0x500`, loads the kernel with INT 13h (sector by sector, crossing tracks), enters protected mode, copies the kernel to 1 MiB. |
| Entry | `entry.S`, `linker.ld` | `_kernel_start` is forced to be the first code after the Multiboot header, so the bootloader can jump to a fixed address. |
| Console | `console.c` | VGA text driver: scrolling region, backspace, tabs, colors, hardware cursor, reserved status row 0. Mirrors output to COM1. |
| Printf | `printf.c` | One formatter (`%c %s %d %u %x %X %p %%`, `-`/`0` flags, widths) behind `console_printf`, `ksnprintf`, `klog`. |
| Serial | `serial.c` | COM1 output, used for headless testing (`qemu -serial`). |
| Log | `klog.c` | Timestamped log levels, 4 KiB ring buffer (`dmesg`). WARN and above also reach the screen. |
| Strings | `kstring.c` | `k*` string helpers plus `memcpy/memset/memmove/memcmp` (the compiler emits calls to these). |
| GDT/TSS | `gdt.c` | Flat ring 0 / ring 3 segments and the TSS used for privilege switches. |
| IDT | `idt.c`, `isr.S` | 48 interrupt stubs + `int 0x80`, exception reporting, IRQ dispatch table. |
| PIC / PIT | `pic.c`, `timer.c` | IRQ remap to 0x20-0x2F, 100 Hz timer, tick counter. |
| Keyboard | `keyboard.c` | Scancode set 1 to ASCII/key codes (shift, caps, ctrl, arrows) into a ring buffer. Polling before the IDT exists, IRQ1 afterwards. |
| PMM | `pmm.c` | Bitmap frame allocator over usable E820 RAM (first 64 MiB). Contiguous allocation and fixed-range reservation. |
| Heap | `kmalloc.c` | `kmalloc/kfree` over a 4 MiB arena: headers with neighbour sizes, O(1) coalescing, next-fit rover, self-test. |
| Scheduler | `sched.c`, `switch.S` | Task control blocks, round-robin with per-priority quantum, sleeping, idle task, reaping, timer-driven preemption. |
| ATA | `ata.c` | PIO LBA28 driver for the primary master. |
| Filesystem | `fs.c` | TinyFS: superblock, 32-entry directory, contiguous files, error codes. |
| User mode | `user.c`, `syscall.c`, `builtin.S`, `user/*.asm` | Program loader (disk first, then built-in), ring 3 entry/exit, syscall table. |
| Debug | `debug.c` | `panic`, `ASSERT`, register dump, hexdump. |
| Shell | `shell.c` | Line editor with history, quoting parser, command table. |

## Later additions

| Module | Files | Role |
| --- | --- | --- |
| Paging | `paging.c` | identity + higher-half mapping, per-task directories, page-fault decoding, guard pages |
| Slab / canaries | `slab.c`, `kmalloc.c` | fixed-size object caches; heap overflow detection |
| Sync | `sync.c` | mutex, semaphore, IRQ-safe spinlock on the scheduler's wait queues |
| VFS | `vfs.c`, `devfs.c`, `procfs.c`, `fat12.c`, `file.c`, `bcache.c` | mounts, path normalization, /dev, /proc, read-only FAT12, descriptor table, write-back cache |
| ELF / user libc | `elf.c`, `user/lib/*` | ELF32 loader, crt0, syscalls, string/stdio/stdlib |
| Hardware | `pci.c`, `cpu.c`, `rtc.c`, `speaker.c`, `acpi.c`, `mouse.c` | discovery and small drivers |
| Network | `rtl8139.c`, `arp.c`, `ip.c`, `icmp.c`, `udp.c`, `dhcp.c`, `tcp.c` | NIC driver and protocol stack |
| Graphics | `vga.c`, `gfx.c`, `gfxcon.c`, `wm.c`, `gui.c`, `editor.c` | mode 13h, drawing, graphical console, windows, buttons, text editor |
| Tooling | `cmdline.c`, `selftest.c` | kernel parameters, test runner |

## Memory map

| Range | Use |
| --- | --- |
| `0x00000500` | E820 table from the bootloader (magic, count, 24-byte entries) |
| `0x00007C00` | boot sector |
| `0x00010000` | temporary kernel load buffer |
| `0x00090000` | kernel boot stack (task 0 / shell) |
| `0x00100000` | kernel image (physical), then `.bss`, then the heap arena |
| `0xC0000000` | higher half: the kernel is linked at `0xC0100000`; low RAM is also mapped here (alias of `0x00000000`) |
| `0x00800000` | user program window (128 KiB: image at the bottom, stack at the top), backed by private frames in a per-program address space |

## Syscalls (`int 0x80`, eax = number)

`1 exit(code)`, `2 write(ptr,len)`, `3 putchar(c)`, `4 ticks()`, `5 getkey()`, `6 sleep(ms)`.

## Disk layout (TinyFS v3)

Sector 0 superblock (`TFS1`, version 3, geometry), then a free-space bitmap (1 bit per sector, sized to the disk), then the entry table (128 x 32-byte entries: name[20], start, size, flags; flags carry used/directory bits and the parent entry index), then data. Files are contiguous; allocation is first-fit over the bitmap, and a rewrite that no longer fits is relocated. Directories are entries without data, so paths like `a/b/c.txt` resolve component by component.

Older layouts are not mountable; run `format` once.

## Added since (batches 03-06)

| Module | Files | Role |
| --- | --- | --- |
| Copy-on-write, demand paging | `paging.c`, `pmm.c`, `user.c` | per-frame reference counts, `PTE_COW`, `fork`, lazy stack and heap |
| IPC and memory sharing | `shm.c`, `mmapf.c`, `pipe.c`, `pcache.c` | shared memory, file mappings, blocking pipes, page cache; OOM handling in `oom.c` |
| Debugging | `ksym.c`, `crashdump.c`, `gdbstub.c`, `debug.c` | symbol table, crash dump on disk, GDB remote stub on COM2, stack traces |
| Shell language | `shell.c`, `script.c`, `arith.c`, `textutil.c` | pipelines, redirection, jobs, scripts, functions, arithmetic, grep/wc/sort/find/diff |
| Users | `users.c`, `cred.c`, `sha256.c` | `/etc/passwd`, login, salted password hashes, current credentials |
| Storage | `blk.c`, `blkcmd.c`, `ext2.c`, `fat12.c`, `ata.c` (DMA), `fs.c` (v5: permissions, links, journal) | block layer, partitions, ext2 and FAT16 reading, FAT12 writing, bus-master DMA |
| Discovery | `ahci.c`, `usb.c`, `usbhid.c` | AHCI ports, USB controllers, HID keyboard report decoder |
| Network | `dns.c`, `tcp.c` (rewritten), `ip.c` (fragments), `sock.c`, `ntp.c`, `telnetd.c`, `udpdemo.c`, `netstat.c` | resolver, reliable TCP, sockets, time sync, remote shell, statistics |
| Audio | `sb.c` | Sound Blaster detection and 8-bit PCM playback over ISA DMA |

See `docs/storage.md`, `docs/ahci.md`, `docs/usb.md`, `docs/cow.md` and `docs/ROADMAP.md` for the details.
