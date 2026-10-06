# Copy-on-write notes

`fork()` conceptually duplicates a process's whole address space. Copying every page up front is slow and mostly wasted (the child often calls `exec` at once). **Copy-on-write (COW)** defers the copy:

1. Instead of copying, the parent's and child's page tables both point at the *same* physical frames.
2. Every shared writable page is marked **read-only** in both tables, and each frame carries a **reference count** (how many address spaces map it).
3. The first write by either side raises a **page fault** (present, write, error code bit 1). The handler checks that the page is a COW page (a software-defined PTE bit, `PTE_COW`, taken from the three bits the CPU leaves to the OS: 9-11), then:
   - if the frame's refcount is 1, nobody else shares it: just make the entry writable again;
   - otherwise allocate a new frame, copy 4 KiB, point the faulting table's entry at the copy as writable, and decrement the original's refcount.
4. `invlpg` the address so the CPU forgets the old read-only translation.

What it needs from the kernel: a per-frame reference count in the frame allocator (freeing a frame only when the count reaches zero), a spare PTE bit to remember "was writable, is shared", and a page-fault handler that can resolve a fault and return so the faulting instruction retries.

The same machinery gives **demand paging** (map nothing, allocate a zeroed frame on first touch) and **shared memory** (map the same frame in several spaces, refcount keeps it alive).
