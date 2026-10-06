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

/* Exception 14: decode CR2 + error code; kills a faulting user program, panics on kernel faults. */
struct regs;
void     paging_fault(struct regs *r);

#endif
