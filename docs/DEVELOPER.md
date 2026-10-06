# Developer guide

## Toolchain

`gcc` (32-bit capable, MinGW-w64 here), `nasm`, `ld`/`objcopy`, `make`, `dd`, `qemu-system-i386`. Python 3 is only needed for `tools/qemu_drive.py`.

The kernel is linked as a PE image (`-m i386pe`) because that is what MinGW's `ld` supports, then flattened with `objcopy -O binary`. C symbols carry a leading underscore, so assembly files refer to `_kernel_main`, `_isr128`, etc. Compile flags include `-mgeneral-regs-only`: SSE is never enabled, and without that flag GCC will happily emit SSE for struct copies and the kernel dies with an invalid-opcode fault.

## Build and run

```bash
make                 # build/os-image.bin (+ build/hello.bin etc.)
make run             # QEMU window, floppy = OS, IDE = build/disk.img, serial log in build/serial.log
make run-headless    # same, no window
make clean-disk      # forget everything stored on the data disk
```

`KERNEL_SECTORS` in the Makefile (default 896 = 448 KiB) is passed to the bootloader; the build fails if `kernel.bin` outgrows it.

## Automated driving

`tools/qemu_drive.py` boots the image headless, types into it through the QEMU monitor and prints the serial log:

```bash
python tools/qemu_drive.py --fresh-disk "format" "write a.txt hi" "cat a.txt"
python tools/qemu_drive.py --screenshot docs/shot.ppm "demo" "wait:14"
```

Tokens: plain text is typed followed by Enter; `raw:text` types without Enter; `key:up` / `key:ctrl-l` send a single key; `wait:N` sleeps.

## Debugging helpers

- `dmesg` shows the kernel log; everything is also on COM1.
- `hexdump <addr> [len]`, `disk <lba>`, `ps`, `meminfo`, `memtest`, `fstest`.
- `panic()` / `ASSERT()` halt with a message; CPU exceptions print a register dump.
- A fault in a user program is contained: the program is killed and the shell continues.

## Adding things

- **Shell command:** write `static int cmd_x(int argc, char **argv)` in `shell.c` and add a row to `commands[]`.
- **Built-in user program:** drop `user/foo.asm` (`bits 32`, `org 0x800000`) or `user/c/foo.c` in place; the Makefile embeds and registers it.
- **IRQ driver:** `irq_install_handler(n, fn)` then `pic_unmask(n)`. The EOI is sent before your handler runs.

## Conventions

Tabs, kernel-style braces, `k`-prefixed helpers, comments only for the non-obvious. Shared state touched by IRQs or by several tasks is guarded with `irq_save()` / `irq_restore()`; there are no other locks.

## User-space C programs

`user/c/NAME.c` is compiled with `UCFLAGS`, linked with `user/lib/crt0.S` (entry `_start`, calls `main(argc, argv, envp)` and `exit`) and `user/lib/ulib.c`, linked at `0x800000` and converted to an ELF (`build/c_NAME.elf`). It is picked up automatically: the Makefile discovers programs by file name (`user/NAME.asm` flat binary, `user/NAME.elf.asm` assembly ELF, `user/c/NAME.c` C ELF) and `tools/genprogs.sh` generates `build/progs_gen.c`, which embeds every image in the kernel and registers it by name. Just add the file and `make`; `install` lists it and `run NAME` starts it. System calls are inline wrappers in `user/lib/usys.h`.

## Testing

- `selftest` inside the OS runs 26 checks (allocators, paging, locks, scheduler, user programs, filesystem, network loopback) and prints PASS/FAIL per test.
- `make check` (`tools/ci.sh`) does a clean build that must be warning-free, boots the kernel, and runs `selftest` headless through QEMU; it exits non-zero on any problem. `tools/ci.sh --watch` re-runs the quick build+boot whenever a source file changes.
- `tools/qemu_drive.py` understands extra tokens: `--menu N` (boot menu entry), `mon:<qemu monitor command>` (for example `mon:screendump file.ppm`, `mon:mouse_move 10 5`), `ser:<text>` (type on the serial port), `hostudp:<text>`, `hostlisten:<port>`, `hosttcp:<port>:<reply>` (talk to the guest over the user-mode network). `tools/ppm2png.py` converts screenshots.
- The Makefile attaches `build/disk.img` (TinyFS, IDE master), `build/fat.img` (FAT12 sample, IDE slave) and an RTL8139 NIC on QEMU's user-mode network (guest `10.0.2.15`, gateway `10.0.2.2`).
