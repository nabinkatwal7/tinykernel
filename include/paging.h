#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PTE_P  0x001u /* present */
#define PTE_RW 0x002u /* writable */
#define PTE_US 0x004u /* user accessible */
#define PTE_COW 0x200u /* software bit: shared copy-on-write page (was writable, now read-only) */
#define PTE_PCD 0x010u /* cache disable: for memory-mapped device registers */

/* Identity-maps the first 64 MiB (virtual == physical) and turns paging on. */
void     paging_init(void);
int      paging_enabled(void);
uint32_t paging_kernel_dir(void);

/* Mapping API. 'dir' is a page directory's physical address (0 = the current one). */
#define PAGING_NOT_MAPPED 0xFFFFFFFFu
int      paging_map(uint32_t dir, uint32_t virt, uint32_t phys, uint32_t flags); /* 0 = ok */
int      paging_unmap(uint32_t dir, uint32_t virt);                              /* 0 = ok */
uint32_t paging_translate(uint32_t dir, uint32_t virt);  /* physical address or PAGING_NOT_MAPPED */
int      paging_is_mapped(uint32_t virt);                /* in the current address space */

/* Exception 14: decode CR2 + error code; kills a faulting user program, panics on kernel faults. */
struct regs;
void     paging_fault(struct regs *r);

/* Address spaces. A new directory shares every kernel mapping with the kernel directory. */
uint32_t paging_fork_dir(uint32_t dir);     /* copy-on-write clone of an address space (0 = out of memory) */
int      paging_cow_fault(uint32_t addr, uint32_t err_code); /* 1 if the fault was a COW write and is now resolved */
uint32_t *paging_pte(uint32_t dir, uint32_t virt);           /* the page table entry mapping virt, or NULL */
void     paging_destroy_user(uint32_t dir);  /* free a user space: private page tables, the user frames they map (refcounted), the directory */
uint32_t paging_new_dir(void);              /* 0 on out-of-memory */
void     paging_free_dir(uint32_t dir);     /* frees the directory and any private tables */
void     paging_switch(uint32_t dir);       /* load CR3 (no-op if already current) */

#endif
