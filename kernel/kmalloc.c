#include "kmalloc.h"

#include "io.h"
#include "klog.h"
#include "kstring.h"
#include "pmm.h"

/*
 * One contiguous arena carved into physically adjacent blocks. Each block has a 16-byte header
 * holding its size and the size of its predecessor, so free() can coalesce with both neighbours
 * in O(1). Allocation is next-fit: a rover remembers where the last search stopped, which avoids
 * rescanning the (usually fragmented) start of the arena.
 */
#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF4EEB10Cu
#define HDR        16u
#define ALIGN      16u
#define CANARY     0xDEADC0DEu

struct block {
	uint32_t magic;
	uint32_t size;      /* payload bytes */
	uint32_t prev_size; /* payload bytes of the previous block, 0 for the first */
	uint32_t req;       /* bytes the caller asked for; a canary follows them */
};

static uint8_t *arena;
static uint32_t arena_size;
static struct block *rover;

static struct block *next_of(struct block *b)
{
	uint8_t *n = (uint8_t *)b + HDR + b->size;

	return n < arena + arena_size ? (struct block *)n : 0;
}

static struct block *prev_of(struct block *b)
{
	return b == (struct block *)arena ? 0
		: (struct block *)((uint8_t *)b - HDR - b->prev_size);
}

void heap_init(void)
{
	static const uint32_t tries[] = { 1024, 512, 256, 64 }; /* frames: 4 MiB .. 256 KiB */
	uint32_t i, base = 0, frames = 0;
	struct block *b;

	for (i = 0; i < 4 && !base; i++) {
		frames = tries[i];
		base = pmm_alloc_contig(frames);
	}
	if (!base) {
		klog(LOG_ERROR, "heap: no memory for the kernel heap");
		return;
	}
	arena = (uint8_t *)base;
	arena_size = frames * PAGE_SIZE;

	b = (struct block *)arena;
	b->magic = MAGIC_FREE;
	b->size = arena_size - HDR;
	b->prev_size = 0;
	rover = b;
	klog(LOG_INFO, "heap: %u KiB arena at %x", arena_size / 1024, base);
}

void *kmalloc(size_t size)
{
	uint32_t flags, need = ((uint32_t)size + sizeof(uint32_t) + ALIGN - 1) & ~(ALIGN - 1);
	struct block *b, *start;

	if (!arena || size == 0 || need > arena_size)
		return 0;

	flags = irq_save();
	b = start = rover;
	do {
		if (b->magic == MAGIC_FREE && b->size >= need) {
			if (b->size >= need + HDR + ALIGN) { /* split off the tail */
				struct block *tail = (struct block *)((uint8_t *)b + HDR + need);
				struct block *after;

				tail->magic = MAGIC_FREE;
				tail->size = b->size - need - HDR;
				tail->prev_size = need;
				b->size = need;
				after = next_of(tail);
				if (after)
					after->prev_size = tail->size;
			}
			b->magic = MAGIC_USED;
			b->req = (uint32_t)size;
			*(uint32_t *)((uint8_t *)b + HDR + size) = CANARY;
			rover = next_of(b) ? next_of(b) : (struct block *)arena;
			irq_restore(flags);
			return (uint8_t *)b + HDR;
		}
		b = next_of(b);
		if (!b)
			b = (struct block *)arena;
	} while (b != start);

	irq_restore(flags);
	return 0;
}

/* 0 if the block's bookkeeping and trailing canary are intact. */
static int block_damaged(struct block *b)
{
	if (b->req + sizeof(uint32_t) > b->size)
		return 1;
	return *(uint32_t *)((uint8_t *)b + HDR + b->req) != CANARY;
}

void *kcalloc(size_t n, size_t size)
{
	void *p = kmalloc(n * size);

	if (p)
		memset(p, 0, n * size);
	return p;
}

void kfree(void *p)
{
	struct block *b, *n, *pr;
	uint32_t flags;

	if (!p)
		return;
	b = (struct block *)((uint8_t *)p - HDR);
	if ((uint8_t *)b < arena || (uint8_t *)b >= arena + arena_size
	    || b->magic != MAGIC_USED) {
		klog(LOG_ERROR, "kfree(%p): bad or double free", p);
		return;
	}

	flags = irq_save();
	if (block_damaged(b))
		klog(LOG_ERROR, "heap: block %p corrupted (overflow or underflow), freeing anyway", p);
	b->magic = MAGIC_FREE;

	n = next_of(b);
	if (n && n->magic == MAGIC_FREE) { /* absorb the next block */
		struct block *nn;

		if (rover == n)
			rover = b;
		b->size += HDR + n->size;
		n->magic = 0;
		nn = next_of(b);
		if (nn)
			nn->prev_size = b->size;
	}
	pr = prev_of(b);
	if (pr && pr->magic == MAGIC_FREE) { /* merge into the previous block */
		struct block *nn;

		if (rover == b)
			rover = pr;
		pr->size += HDR + b->size;
		b->magic = 0;
		nn = next_of(pr);
		if (nn)
			nn->prev_size = pr->size;
	}
	irq_restore(flags);
}

void heap_stats(struct heap_stats *s)
{
	uint32_t flags = irq_save();
	struct block *b;

	memset(s, 0, sizeof *s);
	s->total = arena_size;
	for (b = (struct block *)arena; b; b = next_of(b)) {
		s->blocks++;
		if (b->magic == MAGIC_FREE) {
			s->free += b->size;
			s->free_blocks++;
			if (b->size > s->largest_free)
				s->largest_free = b->size;
		} else {
			s->used += b->size;
		}
	}
	irq_restore(flags);
}

int heap_check(void)
{
	uint32_t flags = irq_save();
	struct block *b, *prev = 0;
	int errors = 0;

	for (b = (struct block *)arena; b; prev = b, b = next_of(b)) {
		if (b->magic != MAGIC_FREE && b->magic != MAGIC_USED)
			errors++;
		if (prev && b->prev_size != prev->size)
			errors++;
		if (prev && prev->magic == MAGIC_FREE && b->magic == MAGIC_FREE)
			errors++; /* two adjacent free blocks: coalescing failed */
		if (b->magic == MAGIC_USED && block_damaged(b))
			errors++; /* overflow into the canary or clobbered header */
	}
	irq_restore(flags);
	return errors;
}

/* Deliberately overflows/underflows blocks and checks that the detector notices. 0 = pass. */
int kmalloc_detector_selftest(void)
{
	uint8_t *a = kmalloc(24), *b = kmalloc(24);
	int bad = 0;

	if (!a || !b)
		return 1;
	if (heap_check())
		bad++;           /* clean heap must report clean */
	a[24] = 0xFF;            /* one byte past the end: trashes the canary */
	if (heap_check() != 1)
		bad++;
	a[24] = (uint8_t)(CANARY & 0xFF); /* repair (little endian low byte) */
	if (heap_check())
		bad++;
	*(uint32_t *)(b - 4) = 0xFFFFFFFFu; /* underflow: clobber the header's request size */
	if (heap_check() != 1)
		bad++;
	*(uint32_t *)(b - 4) = 24;
	kfree(a);
	kfree(b);
	return bad + heap_check();
}

#define ST_SLOTS 48
#define ST_OPS   4000

static uint32_t rng_state = 12345;

static uint32_t rng(void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return (rng_state >> 16) & 0x7FFF;
}

/* Random alloc/free churn with pattern verification. */
int kmalloc_selftest(void)
{
	static void *slot[ST_SLOTS];
	static uint32_t len[ST_SLOTS];
	static uint8_t pat[ST_SLOTS];
	struct heap_stats before, after;
	int bad = 0;
	uint32_t i, j, k;

	heap_stats(&before);
	for (i = 0; i < ST_SLOTS; i++)
		slot[i] = 0;

	for (k = 0; k < ST_OPS; k++) {
		i = rng() % ST_SLOTS;
		if (slot[i]) {
			uint8_t *p = slot[i];

			for (j = 0; j < len[i]; j++)
				if (p[j] != pat[i]) {
					bad++;
					break;
				}
			kfree(p);
			slot[i] = 0;
		} else {
			len[i] = 1 + rng() % 2000;
			pat[i] = (uint8_t)rng();
			slot[i] = kmalloc(len[i]);
			if (slot[i])
				memset(slot[i], pat[i], len[i]);
		}
		if (k % 500 == 0 && heap_check())
			bad++;
	}
	for (i = 0; i < ST_SLOTS; i++)
		kfree(slot[i]);

	heap_stats(&after);
	if (heap_check() || after.used != before.used || after.free_blocks != before.free_blocks)
		bad++;
	return bad;
}
