#include "bcache.h"

#include "ata.h"
#include "io.h"
#include "kmalloc.h"
#include "kstring.h"

#define BC_ENTRIES 64

struct entry {
	uint32_t lba;
	uint32_t stamp;  /* last use, for LRU eviction */
	int valid;
	uint8_t data[SECTOR_SIZE];
};

static struct entry *cache;
static uint32_t clock_tick, hits, misses, writes;

static int ensure(void)
{
	if (!cache)
		cache = kcalloc(BC_ENTRIES, sizeof *cache);
	return cache != 0;
}

static struct entry *find(uint32_t lba)
{
	int i;

	for (i = 0; i < BC_ENTRIES; i++)
		if (cache[i].valid && cache[i].lba == lba)
			return &cache[i];
	return 0;
}

/* A free slot, else the least recently used one. */
static struct entry *victim(void)
{
	struct entry *best = &cache[0];
	int i;

	for (i = 0; i < BC_ENTRIES; i++) {
		if (!cache[i].valid)
			return &cache[i];
		if (cache[i].stamp < best->stamp)
			best = &cache[i];
	}
	return best;
}

int bc_read(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t *out = buf;
	uint32_t i;

	if (!ensure())
		return ata_read(lba, count, buf);
	for (i = 0; i < count; i++) {
		struct entry *e = find(lba + i);

		if (e) {
			hits++;
		} else {
			misses++;
			e = victim();
			if (ata_read(lba + i, 1, e->data))
				return -1;
			e->lba = lba + i;
			e->valid = 1;
		}
		e->stamp = ++clock_tick;
		memcpy(out + i * SECTOR_SIZE, e->data, SECTOR_SIZE);
	}
	return 0;
}

int bc_write(uint32_t lba, uint32_t count, const void *buf)
{
	const uint8_t *in = buf;
	uint32_t i;

	if (!ensure())
		return ata_write(lba, count, buf);
	for (i = 0; i < count; i++) {
		struct entry *e = find(lba + i);

		if (ata_write(lba + i, 1, in + i * SECTOR_SIZE)) /* write-through */
			return -1;
		writes++;
		if (!e) {
			e = victim();
			e->lba = lba + i;
			e->valid = 1;
		}
		memcpy(e->data, in + i * SECTOR_SIZE, SECTOR_SIZE);
		e->stamp = ++clock_tick;
	}
	return 0;
}

void bc_stats_get(struct bc_stats *s)
{
	int i;

	memset(s, 0, sizeof *s);
	s->hits = hits;
	s->misses = misses;
	s->writes = writes;
	s->capacity = BC_ENTRIES;
	if (cache)
		for (i = 0; i < BC_ENTRIES; i++)
			s->entries += cache[i].valid;
}

void bc_invalidate(void)
{
	if (cache)
		memset(cache, 0, BC_ENTRIES * sizeof *cache);
}
