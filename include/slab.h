#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>

/*
 * Slab allocator for fixed-size kernel objects. Each slab is one physical frame: a small header
 * followed by equally sized slots chained on a free list. Freeing finds the slab by masking the
 * object address, so no per-object bookkeeping is needed. Empty slabs go back to the frame
 * allocator (the cache keeps one spare to avoid thrashing).
 */
struct slab;

struct slab_cache {
	const char *name;
	uint32_t objsize;     /* rounded up to 8 bytes */
	uint32_t per_slab;
	struct slab *slabs;   /* slabs with at least one free slot first */
	uint32_t nslabs, allocs, frees, in_use;
};

void  slab_cache_init(struct slab_cache *c, const char *name, uint32_t objsize);
void *slab_alloc(struct slab_cache *c);   /* zeroed, NULL when out of memory */
void  slab_free(struct slab_cache *c, void *obj);
void  slab_cache_destroy(struct slab_cache *c); /* releases every slab; objects become invalid */
void  slab_print_stats(void);              /* all caches, for the shell */

#endif
