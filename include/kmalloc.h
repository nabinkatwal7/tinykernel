#ifndef KMALLOC_H
#define KMALLOC_H

#include <stddef.h>
#include <stdint.h>

struct heap_stats {
	uint32_t total, used, free, largest_free, blocks, free_blocks;
};

void  heap_init(void);
void *kmalloc(size_t size);   /* 16-byte aligned, NULL when out of memory */
void *kcalloc(size_t n, size_t size);
void  kfree(void *p);
void  heap_stats(struct heap_stats *s);
int   heap_check(void);       /* walks every block; returns the number of inconsistencies */
int   kmalloc_selftest(void); /* randomized stress test, 0 = pass */

#endif
