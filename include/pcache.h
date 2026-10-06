#ifndef PCACHE_H
#define PCACHE_H

#include <stdint.h>

/*
 * Page cache for whole-file reads. Cached file contents live in page frames taken from the frame
 * allocator, so the cache can always give them back: there is no swap, memory pressure simply drops
 * cached pages (they can be re-read from disk at any time). Writes, deletes and renames through the VFS
 * invalidate the affected file.
 */
#define PCACHE_PAGES 64

int      pcache_read(const char *path, void *buf, uint32_t cap);   /* like vfs_read, but cached; bytes or error */
void     pcache_invalidate(const char *abs_path);                  /* forget one file */
void     pcache_drop_all(void);
uint32_t pcache_shrink(uint32_t pages);                            /* release up to n least-recently-used pages; returns how many */

struct pcache_stats {
	uint32_t hits, misses, pages, invalidations, shrunk;
};
void     pcache_stats(struct pcache_stats *out);

#endif
