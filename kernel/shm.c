#include "shm.h"

#include "io.h"
#include "kstring.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"

static struct {
	int used, key;
	uint32_t pages;
	uint32_t frame[SHM_MAX_PAGES];
	int attaches;
} seg[SHM_MAX_SEGMENTS];

int shm_get(int key, uint32_t size)
{
	uint32_t flags = irq_save(), pages = (size + PAGE_SIZE - 1) / PAGE_SIZE, i;
	int s, free_slot = -1;

	for (s = 0; s < SHM_MAX_SEGMENTS; s++) {
		if (seg[s].used && seg[s].key == key) {
			irq_restore(flags);
			return s;
		}
		if (!seg[s].used && free_slot < 0)
			free_slot = s;
	}
	if (free_slot < 0 || pages == 0 || pages > SHM_MAX_PAGES) {
		irq_restore(flags);
		return -1;
	}
	for (i = 0; i < pages; i++) {
		seg[free_slot].frame[i] = pmm_alloc();
		if (!seg[free_slot].frame[i]) {
			while (i--)
				pmm_free(seg[free_slot].frame[i]);
			irq_restore(flags);
			return -1;
		}
		memset((void *)seg[free_slot].frame[i], 0, PAGE_SIZE);
	}
	seg[free_slot].used = 1;
	seg[free_slot].key = key;
	seg[free_slot].pages = pages;
	seg[free_slot].attaches = 0;
	irq_restore(flags);
	return free_slot;
}

uint32_t shm_attach(int id)
{
	uint32_t base, i;

	if (id < 0 || id >= SHM_MAX_SEGMENTS || !seg[id].used)
		return 0;
	base = SHM_BASE + (uint32_t)id * SHM_SLOT;
	for (i = 0; i < seg[id].pages; i++) {
		if (paging_map(0, base + i * PAGE_SIZE, seg[id].frame[i], PTE_RW | PTE_US | PTE_SHARED))
			return 0;
		pmm_ref(seg[id].frame[i]);   /* each mapping owns a reference, so destroying a process frees it correctly */
	}
	seg[id].attaches++;
	return base;
}

int shm_detach(uint32_t addr)
{
	int id = (int)((addr - SHM_BASE) / SHM_SLOT);
	uint32_t i;

	if (addr < SHM_BASE || (addr - SHM_BASE) % SHM_SLOT || id >= SHM_MAX_SEGMENTS || !seg[id].used)
		return -1;
	for (i = 0; i < seg[id].pages; i++) {
		uint32_t *pte = paging_pte(0, addr + i * PAGE_SIZE);

		if (pte && (*pte & PTE_P)) {
			paging_unmap(0, addr + i * PAGE_SIZE);
			pmm_free(seg[id].frame[i]);
		}
	}
	if (seg[id].attaches)
		seg[id].attaches--;
	return 0;
}

int shm_remove(int key)
{
	int s;
	uint32_t i;

	for (s = 0; s < SHM_MAX_SEGMENTS; s++) {
		if (seg[s].used && seg[s].key == key) {
			for (i = 0; i < seg[s].pages; i++)
				pmm_free(seg[s].frame[i]);   /* the segment's own reference; mappings keep theirs */
			seg[s].used = 0;
			return 0;
		}
	}
	return -1;
}

int shm_info(int index, int *key, uint32_t *size, int *attaches)
{
	if (index < 0 || index >= SHM_MAX_SEGMENTS || !seg[index].used)
		return -1;
	*key = seg[index].key;
	*size = seg[index].pages * PAGE_SIZE;
	*attaches = seg[index].attaches;
	return 0;
}
