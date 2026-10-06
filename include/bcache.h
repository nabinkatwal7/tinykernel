#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>

/* Sector cache in front of the ATA driver (LRU, write-through). */
struct bc_stats {
	uint32_t hits, misses, writes, entries, capacity;
};

int  bc_read(uint32_t lba, uint32_t count, void *buf);        /* 0 on success, like ata_read */
int  bc_write(uint32_t lba, uint32_t count, const void *buf); /* 0 on success */
void bc_stats_get(struct bc_stats *s);
void bc_invalidate(void);                                      /* forget everything cached */

#endif
