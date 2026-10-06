#include "bcache.h"

#include "ata.h"
#include "io.h"
#include "kmalloc.h"
#include "kstring.h"
#include "sched.h"
#include "sync.h"

#define BC_ENTRIES   64
#define FLUSH_PERIOD 3000 /* ms between background flushes */

struct entry {
	uint32_t lba;
	uint32_t stamp;  /* last use, for LRU eviction */
	int valid;
	int dirty;       /* newer than the disk: must be written back before reuse */
	uint8_t data[SECTOR_SIZE];
};

static struct entry *cache;
static uint32_t clock_tick, hits, misses, writes, writebacks;
static mutex_t lock = MUTEX_INIT; /* the flusher task and the shell share the cache */

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

/* A free slot, else the least recently used one (its dirty data is written out first). */
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
	if (best->dirty) {
		if (ata_write(best->lba, 1, best->data))
			return 0;
		best->dirty = 0;
		writebacks++;
	}
	return best;
}

static int read_locked(uint32_t lba, uint32_t count, uint8_t *out)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		struct entry *e = find(lba + i);

		if (e) {
			hits++;
		} else {
			misses++;
			e = victim();
			if (!e || ata_read(lba + i, 1, e->data))
				return -1;
			e->lba = lba + i;
			e->valid = 1;
			e->dirty = 0;
		}
		e->stamp = ++clock_tick;
		memcpy(out + i * SECTOR_SIZE, e->data, SECTOR_SIZE);
	}
	return 0;
}

int bc_read(uint32_t lba, uint32_t count, void *buf)
{
	int rc;

	if (!ensure())
		return ata_read(lba, count, buf);
	mutex_lock(&lock);
	rc = read_locked(lba, count, buf);
	mutex_unlock(&lock);
	return rc;
}

/* Write-back: the data only reaches the cache; bc_flush() (or eviction) puts it on the disk. */
int bc_write(uint32_t lba, uint32_t count, const void *buf)
{
	const uint8_t *in = buf;
	uint32_t i;

	if (!ensure())
		return ata_write(lba, count, buf);
	mutex_lock(&lock);
	for (i = 0; i < count; i++) {
		struct entry *e = find(lba + i);

		if (!e) {
			e = victim();
			if (!e) {
				mutex_unlock(&lock);
				return -1;
			}
			e->lba = lba + i;
			e->valid = 1;
		}
		memcpy(e->data, in + i * SECTOR_SIZE, SECTOR_SIZE);
		e->dirty = 1;
		e->stamp = ++clock_tick;
		writes++;
	}
	mutex_unlock(&lock);
	return 0;
}

int bc_flush(void)
{
	int i, n = 0;

	if (!cache)
		return 0;
	mutex_lock(&lock);
	for (i = 0; i < BC_ENTRIES; i++) {
		if (cache[i].valid && cache[i].dirty) {
			if (ata_write(cache[i].lba, 1, cache[i].data)) {
				mutex_unlock(&lock);
				return -1;
			}
			cache[i].dirty = 0;
			writebacks++;
			n++;
		}
	}
	mutex_unlock(&lock);
	return n;
}

void bc_stats_get(struct bc_stats *s)
{
	int i;

	memset(s, 0, sizeof *s);
	s->hits = hits;
	s->misses = misses;
	s->writes = writes;
	s->writebacks = writebacks;
	s->capacity = BC_ENTRIES;
	if (cache) {
		for (i = 0; i < BC_ENTRIES; i++) {
			s->entries += cache[i].valid;
			s->dirty += cache[i].valid && cache[i].dirty;
		}
	}
}

/* Writes everything out first, so dropping the cache can never lose data. */
void bc_invalidate(void)
{
	bc_flush();
	if (cache) {
		mutex_lock(&lock);
		memset(cache, 0, BC_ENTRIES * sizeof *cache);
		mutex_unlock(&lock);
	}
}

static void flusher(void *arg)
{
	(void)arg;
	for (;;) {
		task_sleep(FLUSH_PERIOD);
		bc_flush();
	}
}

void bc_start_flusher(void)
{
	task_create("flusher", flusher, 0, 3);
}
