# Final review and the 300-day roadmap

## Where the system stands

Tiny OS is a 32-bit x86 hobby kernel that boots from a BIOS boot sector, runs in the higher half with paging, schedules
tasks on several CPUs, loads ELF programs into ring 3, mounts several filesystems, and talks TCP/IP. The three
backlogs (78 + 100 + 100 + 28 items) are finished; `make check` builds without warnings, boots in QEMU and runs the
38 self-tests.

| Area | What exists |
| --- | --- |
| Boot / CPU | BIOS loader with E820, GDT/TSS/IDT, LAPIC timer calibrated against the PIT, ACPI/MADT, SMP with per-CPU run queues |
| Memory | frame allocator with reference counts, paging with copy-on-write `fork`, demand-paged stack and heap, shared memory, `mmap`, page cache, OOM killer, kmalloc with canaries, slab caches |
| Scheduling | priorities with aging, wait queues, mutex / semaphore / spin / ticket locks, kernel tasks and background jobs |
| Processes | ELF and flat programs, about 45 syscalls (files, pipes, sockets, locks, graphics, time), `fork`/`waitpid`, descriptors with `dup`, pipes and redirection |
| Storage | block layer (ATA with DMA, RAM disk, loop devices, MBR partitions), TinyFS v5 (permissions, owners, times, symlinks, hard links, journal), FAT12 read-write, FAT16 and ext2 read-only, devfs, procfs |
| Users | `/etc/passwd` with salted SHA-256, login, `su`, `chmod`, `chown`, per-file permission checks |
| Network | RTL8139 with a transmit queue, ARP with aging, IPv4 with fragmentation, ICMP, UDP, TCP (listen/accept, retransmission, sliding window), DHCP, DNS, SNTP, sockets, HTTP client/server, remote shell |
| Shell | pipelines, redirection, `&` jobs, scripts with `if`/`while`/functions, aliases, variables, arithmetic, tab completion, cursor editing, text tools |
| Devices | VGA text and mode 13h, window manager, PS/2 mouse, serial console, PC speaker, Sound Blaster PCM, PCI, AHCI and USB discovery |
| Debugging | symbolised stack traces, crash dumps on disk, GDB stub on COM2, assertions, `selftest`, fault injection (`tcpstat drop`, `jtest`) |

## What is deliberately missing or weak

These are the honest gaps, in rough order of how much they limit the system:

1. **One address space per program, one program at a time** from the shell. `fork` children are tasks, but there is no
   `exec` into a new process table entry and no per-process descriptor table (descriptors are global).
2. **No real disk drivers beyond ATA PIO/DMA.** AHCI and USB are only detected; there is no write support for ext2 or FAT16.
3. **TCP has no congestion control, no options (window scale, SACK, timestamps) and skips TIME_WAIT.** Out-of-order
   segments are dropped, not queued.
4. **The VFS is whole-file**: reads and writes move complete files through buffers (up to 256 KiB). Fine for the
   current programs, wrong for databases and big media.
5. **Security is shallow.** Permission checks exist, but the kernel trusts many user pointers beyond range checks, there
   is no ASLR or NX, and the telnet server sends passwords in clear text.
6. **No power management, no ACPI shutdown beyond QEMU's port, no real-hardware testing.** Everything is verified in
   QEMU; timing assumptions (PIT, TSC) may not hold elsewhere.
7. **Single-threaded user programs** with a small libc (no stdio buffering, no `printf` of floating point).

## Roadmap: 300 days in six phases of fifty

Each phase ends with a demonstrable milestone and keeps `make check` green; items are ordered by dependency.

### Phase 1 (days 1-50): processes done properly
* Per-process descriptor tables and a process table (`struct process`) separate from tasks.
* `exec` replacing the image of the current process; `wait`/`kill` on pids; process groups and a controlling terminal.
* Signals: `SIGINT`, `SIGTERM`, `SIGCHLD`, `SIGPIPE` delivered at syscall exit; `sigaction`/`sigreturn`.
* Multiple user programs resident at once (several address spaces, shared kernel mappings), so a shell can run `a | b`
  as real concurrent processes and `&` works for user programs.
* Milestone: a user-mode shell (`ush`) that supports pipes between concurrent programs and job control.

### Phase 2 (days 51-100): a real storage stack
* Page-granular file I/O (`read`/`write`/`seek` through the page cache instead of whole files); extent-based TinyFS v6
  with fragmentation handling.
* ext2 write support, FAT16/FAT32 write support, long file names on FAT.
* AHCI read/write with command lists and NCQ off; interrupts instead of polling for ATA.
* `mount`/`umount` as syscalls with a mount table in `/proc/mounts`; `fsck` for ext2.
* Milestone: boot from a SATA disk, root on ext2, with the TinyFS disk as a data volume.

### Phase 3 (days 101-150): networking to production quality
* TCP: out-of-order queue, SACK, congestion control (slow start, congestion avoidance, fast retransmit), window
  scaling, TIME_WAIT, keep-alive.
* Multiple interfaces and routing table; ARP/NDP, IPv6 basics; DHCP renewal.
* A second NIC driver (e1000) and virtio-net; checksum offload.
* TLS client over a small library (or a port), `wget https://`.
* Milestone: download a 10 MiB file from the internet at line rate, and serve the same through `httpd` concurrently.

### Phase 4 (days 151-200): USB, input and graphics
* xHCI/EHCI host controller driver with enumeration, hubs, HID keyboard and mouse, mass storage (bulk-only).
* Linear framebuffer mode via VBE/Bochs VBE instead of mode 13h; font rendering with a proportional font; double
  buffering; a compositing window manager with real user-mode clients over a socket protocol.
* Audio: SB16 16-bit and stereo, AC97/HDA playback, a mixer, WAV/OGG decoding in user space.
* Milestone: a desktop with a terminal, a file manager and a text editor running as separate processes.

### Phase 5 (days 201-250): security and robustness
* NX and SMEP/SMAP, kernel address-space layout randomisation, stack protector, hardened `copy_from_user`/`copy_to_user`.
* Capabilities or at least `setuid` semantics, `/etc/shadow` split, login lockout, TLS for the remote shell (or SSH).
* Fuzz the syscall layer and the network stack (host-side packet generators, syscall fuzzer in a user program);
  fix everything it finds; run under QEMU with `-d` assertions in CI.
* Kernel memory accounting per process; resource limits.
* Milestone: a hostile user program cannot crash or read the kernel, shown by a written test suite.

### Phase 6 (days 251-300): hardware, tooling and release
* Run on real hardware: legacy-free boot (UEFI via a small stub or GRUB), HPET and TSC-deadline timers, ACPI power
  buttons and S5 shutdown, PS/2 and USB laptop keyboards, an Intel NIC.
* Self-hosting steps: a small C compiler or assembler running on Tiny OS (tcc port), a `make`, an editor with syntax
  highlighting.
* Package format and installer: `tar`-like archive, `pkg install` from an HTTP server, versioned system image builds
  in CI with the GDB stub and crash dumps archived on failure.
* Documentation pass: man-style pages for every command, an architecture book from the docs in this folder.
* Milestone: version 1.0 - boots on a real PC, installs from USB, and rebuilds part of itself.

## How to keep going

* Keep one commit per backlog item and one pull request per batch; `tools/ci.sh --watch` rebuilds on every change.
* Every feature should arrive with a test: a kernel command that returns non-zero on failure (see `jtest`, `fdisk test`,
  `tcpsend`) and, when it needs a second machine, a helper in `tools/qemu_drive.py` (`hostsink`, `hostsrc`, `guesttcp`).
* When the kernel image nears the boot loader's 1150-sector limit, move programs to `/fat/bin` (`FAT_PROGS` in the
  Makefile) or teach the loader to read the kernel in a second stage.
