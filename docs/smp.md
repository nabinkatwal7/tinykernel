# SMP startup notes

On a multiprocessor x86 only one core, the **bootstrap processor (BSP)**, runs at power-on. The others (**application processors, APs**) wait in a halted state until the BSP wakes them with the **INIT-SIPI-SIPI** sequence through the local APIC's interrupt command register (ICR, offsets `0x300` low / `0x310` high):

1. **INIT IPI** to the target APIC id (`0x00004500` | delivery mode INIT, level assert) - resets the core. Wait ~10 ms.
2. **STARTUP IPI** (`0x00004600 | vector`) twice, ~200 us apart. The vector is the physical page number of the entry code: the AP starts executing in **real mode** at `vector * 0x1000`, so the code must sit below 1 MiB, page aligned (for example `0x8000` -> vector `0x08`).
3. The AP then has to do by itself everything the boot sector did for the BSP: load a GDT, set `CR0.PE`, far-jump to 32-bit code, then enable paging with the *same* page directory (`CR3`) and jump into the higher-half kernel.

The trampoline needs a handshake area: a per-AP stack pointer, the kernel page directory, the entry function and a "I am up" flag the BSP can poll.

Each core has its own local APIC (same physical address, banked per core), its own timer, GDT/TSS and IDT register. The page tables and the kernel's data are shared, so every shared structure needs real locking: `cli` only stops interrupts on the *current* core, a spinlock built on `xchg` is what keeps two cores out of the same critical section.

ACPI's MADT lists every processor's local APIC id and whether it is enabled; QEMU starts extra cores with `-smp N`.
