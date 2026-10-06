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

`KERNEL_SECTORS` in the Makefile (default 256 = 128 KiB) is passed to the bootloader; the build fails if `kernel.bin` outgrows it.

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
- **Built-in user program:** add `user/foo.asm` (`bits 32`, `org 0x800000`), add it to `USER_BINS`, `builtin.S` and the table in `user.c`.
- **IRQ driver:** `irq_install_handler(n, fn)` then `pic_unmask(n)`. The EOI is sent before your handler runs.

## Conventions

Tabs, kernel-style braces, `k`-prefixed helpers, comments only for the non-obvious. Shared state touched by IRQs or by several tasks is guarded with `irq_save()` / `irq_restore()`; there are no other locks.
