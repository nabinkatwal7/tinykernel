#include "slab.h"

#include "console.h"
#include "io.h"
#include "klog.h"
#include "kstring.h"
#include "pmm.h"

#define SLAB_MAGIC 0x51AB51ABu
#define MAX_CACHES 8

struct slab {
	uint32_t magic;
	struct slab *next, *prev;
	void *free;          /* singly linked list threaded through free slots */
	uint32_t used;
};

static struct slab_cache *caches[MAX_CACHES];
static int ncaches;

static struct slab *slab_new(struct slab_cache *c)
{
	uint32_t frame = pmm_alloc();
	struct slab *s = (struct slab *)frame;
	uint32_t i, off = (sizeof *s + 7) & ~7u;

	if (!frame)
		return 0;
	memset(s, 0, sizeof *s);
	s->magic = SLAB_MAGIC;
	for (i = c->per_slab; i-- > 0;) { /* build the free list in address order */
		void **slot = (void **)(frame + off + i * c->objsize);

		*slot = s->free;
		s->free = slot;
	}
	s->next = c->slabs;
	if (c->slabs)
		c->slabs->prev = s;
	c->slabs = s;
	c->nslabs++;
	return s;
}

void slab_cache_init(struct slab_cache *c, const char *name, uint32_t objsize)
{
	uint32_t off = (sizeof(struct slab) + 7) & ~7u;
	int i, known = 0;

	memset(c, 0, sizeof *c);
	c->name = name;
	c->objsize = (objsize + 7) & ~7u;
	if (c->objsize < 8)
		c->objsize = 8;
	c->per_slab = (PAGE_SIZE - off) / c->objsize;
	for (i = 0; i < ncaches; i++)
		known |= caches[i] == c;
	if (!known && ncaches < MAX_CACHES)
		caches[ncaches++] = c;
}

void *slab_alloc(struct slab_cache *c)
{
	uint32_t f = irq_save();
	struct slab *s;
	void **obj;

	for (s = c->slabs; s && !s->free; s = s->next)
		;
	if (!s)
		s = slab_new(c);
	if (!s) {
		irq_restore(f);
		return 0;
	}
	obj = s->free;
	s->free = *obj;
	s->used++;
	c->allocs++;
	c->in_use++;
	irq_restore(f);
	memset(obj, 0, c->objsize);
	return obj;
}

void slab_free(struct slab_cache *c, void *obj)
{
	uint32_t f = irq_save();
	struct slab *s = (struct slab *)((uint32_t)obj & ~(PAGE_SIZE - 1));

	if (!obj) {
		irq_restore(f);
		return;
	}
	if (s->magic != SLAB_MAGIC || !s->used) {
		klog(LOG_ERROR, "slab_free(%s): bad object %p", c->name, obj);
		irq_restore(f);
		return;
	}
	*(void **)obj = s->free;
	s->free = obj;
	s->used--;
	c->frees++;
	c->in_use--;

	if (s->used == 0 && c->nslabs > 1) { /* give a fully empty slab back, keep one spare */
		if (s->prev)
			s->prev->next = s->next;
		else
			c->slabs = s->next;
		if (s->next)
			s->next->prev = s->prev;
		s->magic = 0;
		pmm_free((uint32_t)s);
		c->nslabs--;
	}
	irq_restore(f);
}

void slab_cache_destroy(struct slab_cache *c)
{
	uint32_t f = irq_save();

	while (c->slabs) {
		struct slab *s = c->slabs;

		c->slabs = s->next;
		s->magic = 0;
		pmm_free((uint32_t)s);
	}
	c->nslabs = c->in_use = 0;
	irq_restore(f);
}

void slab_print_stats(void)
{
	int i;

	console_write("CACHE        OBJSZ  PER-SLAB  SLABS  IN-USE  ALLOCS  FREES\n");
	for (i = 0; i < ncaches; i++) {
		struct slab_cache *c = caches[i];

		console_printf("%-12s %5u  %8u  %5u  %6u  %6u  %5u\n", c->name, c->objsize,
			       c->per_slab, c->nslabs, c->in_use, c->allocs, c->frees);
	}
}
