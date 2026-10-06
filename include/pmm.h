#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE 4096u

/* Bitmap physical frame allocator over the usable RAM the bootloader found via E820. */
void     pmm_init(void);
uint32_t pmm_alloc(void);                          /* one frame, 0 on failure */
uint32_t pmm_alloc_contig(uint32_t n);             /* n adjacent frames, 0 on failure */
int      pmm_reserve(uint32_t addr, uint32_t n);   /* claim a specific range; 0 on success */
void     pmm_free(uint32_t addr);
void     pmm_free_range(uint32_t addr, uint32_t n);
uint32_t pmm_total_frames(void);
uint32_t pmm_free_frames(void);
void     pmm_print_map(void);                      /* E820 table + allocator summary */

#endif
