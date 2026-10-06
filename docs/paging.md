# Paging notes

x86 paging translates linear addresses through two table levels once `CR0.PG` is set:

```
31        22 21        12 11         0
+-----------+------------+------------+
| dir index | table index|   offset   |
+-----------+------------+------------+
```

- `CR3` holds the physical address of the 4 KiB **page directory** (1024 entries).
- Each directory entry points at a **page table** (1024 entries); each table entry maps one 4 KiB frame.
- Entry bits we use: `P` present (bit 0), `RW` writable (1), `US` user-accessible (2). The upper 20 bits are the frame address.
- A violation raises exception 14; `CR2` holds the faulting address and the error code says present/write/user.
- After changing a mapping run `invlpg <addr>`, or reload `CR3` to flush the whole TLB.

Plan for this kernel: identity-map RAM (virtual == physical) so existing code keeps working, keep kernel pages supervisor-only, and mark only the user program window `US=1`.

## Higher-half layout

The kernel is linked at virtual `0xC0100000` but loaded at physical `0x100000`. The bootloader still jumps to physical `0x10000C` with paging off; `entry.S` builds a temporary 4 MiB-page directory (`boot_pd`) that maps the first 64 MiB both at `0` and at `0xC0000000`, enables paging and jumps to the high alias. `paging_init()` later replaces it with 4 KiB page tables, reusing the same tables for both mappings. The low identity map stays because the allocators hand out physical addresses that the kernel dereferences directly.

(`qemu -kernel` / Multiboot loading no longer works, since the ELF would claim physical addresses in the higher half.)
