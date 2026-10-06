#include "pcache.h"

#include "io.h"
#include "kmalloc.h"
#include "kstring.h"
#include "pmm.h"
#include "vfs.h"

#define MAX_FILE (PCACHE_PAGES * PAGE_SIZE)

static struct {
	int used;
	uint32_t hash, size, index, frame, stamp;
} ent[PCACHE_PAGES];
static uint32_t clock_tick, hits, misses, invalidations, shrunk;

static uint32_t hash_path(const char *p)
{
	uint32_t h = 2166136261u;      /* FNV-1a */

	while (*p) {
		h ^= (uint8_t)*p++;
		h *= 16777619u;
	}
	return h ? h : 1;
}

static void drop_entry(int i)
{
	pmm_free(ent[i].frame);
	ent[i].used = 0;
}

static int find(uint32_t h, uint32_t index)
{
	int i;

	for (i = 0; i < PCACHE_PAGES; i++)
		if (ent[i].used && ent[i].hash == h && ent[i].index == index)
			return i;
	return -1;
}

static int lru(void)
{
	int i, best = -1;

	for (i = 0; i < PCACHE_PAGES; i++)
		if (ent[i].used && (best < 0 || ent[i].stamp < ent[best].stamp))
			best = i;
	return best;
}

/* The path in canonical absolute form, so "a.txt" and "/a.txt" share cache pages. */
static int canonical(const char *path, char out[VFS_PATH_MAX])
{
	return vfs_normalize(vfs_getcwd(), path, out, VFS_PATH_MAX);
}

void pcache_invalidate(const char *abs_path)
{
	uint32_t h = hash_path(abs_path), flags = irq_save();
	int i;

	for (i = 0; i < PCACHE_PAGES; i++) {
		if (ent[i].used && ent[i].hash == h) {
			drop_entry(i);
			invalidations++;
		}
	}
	irq_restore(flags);
}

void pcache_drop_all(void)
{
	uint32_t flags = irq_save();
	int i;

	for (i = 0; i < PCACHE_PAGES; i++)
		if (ent[i].used)
			drop_entry(i);
	irq_restore(flags);
}

uint32_t pcache_shrink(uint32_t pages)
{
	uint32_t n = 0, flags = irq_save();

	while (n < pages) {
		int i = lru();

		if (i < 0)
			break;
		drop_entry(i);
		n++;
	}
	shrunk += n;
	irq_restore(flags);
	return n;
}

void pcache_stats(struct pcache_stats *out)
{
	int i;

	memset(out, 0, sizeof *out);
	out->hits = hits;
	out->misses = misses;
	out->invalidations = invalidations;
	out->shrunk = shrunk;
	for (i = 0; i < PCACHE_PAGES; i++)
		out->pages += ent[i].used;
}

int pcache_read(const char *path, void *buf, uint32_t cap)
{
	char abs[VFS_PATH_MAX];
	uint32_t h, size, i, npages, copied = 0;
	int n;

	n = vfs_size(path);
	if (n < 0)
		return n;
	size = (uint32_t)n;
	if (size > cap)
		return -7; /* FS_ETOOBIG */
	if (size == 0)
		return 0;
	if (size > MAX_FILE || canonical(path, abs))
		return vfs_read(path, buf, cap);       /* too big for the cache: read straight from the filesystem */
	h = hash_path(abs);
	npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

	/* all pages present and the cached size matches? then serve from memory */
	for (i = 0; i < npages; i++) {
		int e = find(h, i);

		if (e < 0 || ent[e].size != size)
			break;
	}
	if (i == npages) {
		hits++;
		for (i = 0; i < npages; i++) {
			int e = find(h, i);
			uint32_t chunk = size - copied < PAGE_SIZE ? size - copied : PAGE_SIZE;

			memcpy((uint8_t *)buf + copied, (void *)ent[e].frame, chunk);
			ent[e].stamp = ++clock_tick;
			copied += chunk;
		}
		return (int)size;
	}

	misses++;
	pcache_invalidate(abs);                    /* drop any stale pages of this file first */
	n = vfs_read(path, buf, cap);
	if (n < 0)
		return n;
	for (i = 0; i < npages; i++) {              /* fill the cache from what we just read */
		uint32_t frame = pmm_alloc(), chunk = size - i * PAGE_SIZE < PAGE_SIZE ? size - i * PAGE_SIZE : PAGE_SIZE;
		int slot = -1, k;

		for (k = 0; k < PCACHE_PAGES; k++)
			if (!ent[k].used) {
				slot = k;
				break;
			}
		if (slot < 0) {                         /* cache full: recycle the least recently used page */
			slot = lru();
			drop_entry(slot);
		}
		if (!frame)
			break;
		memset((void *)frame, 0, PAGE_SIZE);
		memcpy((void *)frame, (uint8_t *)buf + i * PAGE_SIZE, chunk);
		ent[slot].used = 1;
		ent[slot].hash = h;
		ent[slot].size = size;
		ent[slot].index = i;
		ent[slot].frame = frame;
		ent[slot].stamp = ++clock_tick;
	}
	return n;
}
