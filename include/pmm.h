#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PAGE_SIZE 4096u
#define KERNEL_VMA 0xC0000000u /* kernel image and the direct map of low RAM live above this */

/* Bitmap physical frame allocator over the usable RAM the bootloader found via E820. */
void     pmm_init(void);
uint32_t pmm_alloc(void);                          /* one frame, 0 on failure */
uint32_t pmm_alloc_contig(uint32_t n);             /* n adjacent frames, 0 on failure */
int      pmm_reserve(uint32_t addr, uint32_t n);   /* claim a specific range; 0 on success */
void     pmm_free(uint32_t addr);                  /* drops one reference; the frame is released at zero */
void     pmm_ref(uint32_t addr);                   /* one more owner of this frame (copy-on-write, shared memory) */
uint32_t pmm_refcount(uint32_t addr);              /* owners of a frame: 0 = free or kernel-owned */
void     pmm_free_range(uint32_t addr, uint32_t n);
uint32_t pmm_total_frames(void);
uint32_t pmm_free_frames(void);
uint32_t pmm_ram_kib(void);       /* installed RAM from the E820 map (top of the highest usable region, KiB) */
uint32_t pmm_usable_kib(void);    /* sum of all E820 usable regions, KiB */
uint32_t pmm_cmos_ram_kib(void);  /* second opinion from the CMOS memory registers, KiB */
void     pmm_print_map(void);                      /* E820 table + allocator summary */

#endif
