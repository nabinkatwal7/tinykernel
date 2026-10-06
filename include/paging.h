#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PTE_P  0x001u /* present */
#define PTE_RW 0x002u /* writable */
#define PTE_US 0x004u /* user accessible */

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

#endif
