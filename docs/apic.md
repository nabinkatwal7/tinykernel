# APIC notes

The legacy 8259 PIC can only serve one CPU. Modern x86 systems use two kinds of APIC:

- **Local APIC (LAPIC)**: one per CPU core, memory mapped at `0xFEE00000` by default (the address is in MSR `0x1B`, `IA32_APIC_BASE`; bit 11 enables it). It receives interrupts, owns a per-core **timer**, and sends **inter-processor interrupts** (IPIs).
- **I/O APIC**: usually at `0xFEC00000`; it routes the external interrupt lines (keyboard, disk, NIC...) to a CPU's LAPIC as a numbered vector, through a table of 64-bit redirection entries (register index `0x10 + 2n`).

## LAPIC registers (offsets from the base)

| offset | register |
| --- | --- |
| `0x020` | local APIC id (bits 24-31) |
| `0x0B0` | EOI: write 0 to end an interrupt |
| `0x0F0` | spurious interrupt vector; bit 8 = software enable |
| `0x300`/`0x310` | interrupt command register low/high (send IPIs) |
| `0x320` | LVT timer: vector, bit 16 = masked, bit 17 = periodic mode |
| `0x380` / `0x390` | timer initial / current count |
| `0x3E0` | timer divide configuration |

The LAPIC timer counts down at the bus frequency (divided); there is no fixed rate, so calibrate it against the PIT: let it run for a known time and see how far it counted.

## Moving off the PIC

1. Detect the APIC with `CPUID.1:EDX[9]`.
2. Mask every 8259 line (write `0xFF` to both data ports) so it stops delivering interrupts.
3. Enable the LAPIC (spurious vector register, bit 8) and map its page into the kernel.
4. Program the I/O APIC redirection entries for the devices we use, and send EOI to the LAPIC instead of the PIC.
5. Start the LAPIC timer in periodic mode for the scheduler tick.

The MADT table in ACPI lists each CPU's LAPIC id, the I/O APIC address and the interrupt source overrides (for example the timer being wired to I/O APIC input 2).
