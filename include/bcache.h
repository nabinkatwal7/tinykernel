#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>

/*
 * Sector cache in front of the ATA driver: LRU, write-back. Writes only dirty the cache; they
 * reach the disk when bc_flush() runs (sync, shutdown, the background flusher task every 3 s) or
 * when the entry is evicted. A power cut can therefore lose the last few seconds of writes.
 */
struct bc_stats {
	uint32_t hits, misses, writes, writebacks, entries, dirty, capacity;
};

int  bc_read(uint32_t lba, uint32_t count, void *buf);        /* 0 on success, like ata_read */
int  bc_write(uint32_t lba, uint32_t count, const void *buf); /* 0 on success */
int  bc_flush(void);                  /* write every dirty sector out; returns how many, or -1 on error */
void bc_start_flusher(void);          /* background task that flushes periodically */
void bc_stats_get(struct bc_stats *s);
void bc_invalidate(void);                                      /* forget everything cached */

#endif
