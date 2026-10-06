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
