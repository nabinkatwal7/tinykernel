#include "mmapf.h"

#include "kmalloc.h"
#include "kstring.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"
#include "vfs.h"

#define PTE_DIRTY 0x040u

static struct mmap_rec *find(uint32_t addr)
{
	task_t *t = task_current();
	int i;

	for (i = 0; i < MMAP_SLOTS; i++)
		if (t->maps[i].addr == addr && addr)
			return &t->maps[i];
	return 0;
}

uint32_t mmap_file(const char *path, uint32_t length, int flags)
{
	task_t *t = task_current();
	struct mmap_rec *rec = 0;
	uint32_t size, pages, i, base;
	int n;

	for (i = 0; i < MMAP_SLOTS && !rec; i++)
		if (!t->maps[i].addr)
			rec = &t->maps[i];
	if (!rec || kstrlen(path) >= sizeof rec->path)
		return 0;
	n = vfs_size(path);
	if (n < 0)
		return 0;
	size = (uint32_t)n;
	if (!length)
		length = size;
	if (!length || length > MMAP_SLOT)
		return 0;
	pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
	base = MMAP_BASE + (uint32_t)(rec - t->maps) * MMAP_SLOT;

	for (i = 0; i < pages; i++) {
		uint32_t frame = pmm_alloc();

		if (!frame || (memset((void *)frame, 0, PAGE_SIZE), paging_map(0, base + i * PAGE_SIZE, frame, PTE_RW | PTE_US))) {
			while (i--) {
				uint32_t *pte = paging_pte(0, base + i * PAGE_SIZE);

				if (pte) {
					pmm_free(*pte & ~0xFFFu);
					paging_unmap(0, base + i * PAGE_SIZE);
				}
			}
			return 0;
		}
	}
	if (size && vfs_read(path, (void *)base, size > length ? length : size) < 0 && size <= length) {
		/* could not read the file: undo */
		for (i = 0; i < pages; i++) {
			uint32_t *pte = paging_pte(0, base + i * PAGE_SIZE);

			if (pte) {
				pmm_free(*pte & ~0xFFFu);
				paging_unmap(0, base + i * PAGE_SIZE);
			}
		}
		return 0;
	}
	rec->addr = base;
	rec->pages = pages;
	rec->file_size = size;
	rec->shared = flags & MMAP_SHARED;
	kstrlcpy(rec->path, path, sizeof rec->path);
	return base;
}

int mmap_sync(uint32_t addr)
{
	struct mmap_rec *rec = find(addr);
	uint32_t i, dirty = 0, len;

	if (!rec)
		return -1;
	if (!rec->shared)
		return 0;
	for (i = 0; i < rec->pages; i++) {
		uint32_t *pte = paging_pte(0, rec->addr + i * PAGE_SIZE);

		if (pte && (*pte & PTE_DIRTY)) {
			dirty = 1;
			*pte &= ~PTE_DIRTY;
		}
	}
	if (!dirty)
		return 0;
	len = rec->pages * PAGE_SIZE < rec->file_size ? rec->pages * PAGE_SIZE : rec->file_size;
	return vfs_write(rec->path, (const void *)rec->addr, len) ? -1 : 0;
}

int mmap_unmap(uint32_t addr)
{
	struct mmap_rec *rec = find(addr);
	uint32_t i;
	int rc;

	if (!rec)
		return -1;
	rc = mmap_sync(addr);
	for (i = 0; i < rec->pages; i++) {
		uint32_t *pte = paging_pte(0, rec->addr + i * PAGE_SIZE);

		if (pte && (*pte & PTE_P)) {
			uint32_t frame = *pte & ~0xFFFu;

			paging_unmap(0, rec->addr + i * PAGE_SIZE);
			pmm_free(frame);
		}
	}
	memset(rec, 0, sizeof *rec);
	return rc;
}
