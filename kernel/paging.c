#include "paging.h"

#include "klog.h"
#include "kstring.h"
#include "pmm.h"
#include "user.h"

#define MAP_BYTES (64u << 20) /* matches what pmm manages */
#define ENTRIES   1024u

static uint32_t *kdir;
static int enabled;

static uint32_t *alloc_table(void)
{
	uint32_t *t = (uint32_t *)pmm_alloc();

	if (t)
		memset(t, 0, PAGE_SIZE);
	return t;
}

void paging_init(void)
{
	uint32_t addr, i, tables = 0;

	kdir = alloc_table();
	if (!kdir) {
		klog(LOG_ERROR, "paging: out of memory");
		return;
	}
	for (addr = 0; addr < MAP_BYTES; addr += ENTRIES * PAGE_SIZE) {
		uint32_t *t = alloc_table();

		if (!t) {
			klog(LOG_ERROR, "paging: out of memory for page tables");
			return;
		}
		for (i = 0; i < ENTRIES; i++) {
			uint32_t page = addr + i * PAGE_SIZE;
			uint32_t flags = PTE_P | PTE_RW;

			if (page >= USER_BASE && page < USER_END)
				flags |= PTE_US; /* the user program window */
			t[i] = page | flags;
		}
		kdir[addr >> 22] = (uint32_t)t | PTE_P | PTE_RW | PTE_US;
		tables++;
	}

	__asm__ volatile (
		"movl %0, %%cr3\n\t"
		"movl %%cr0, %%eax\n\t"
		"orl $0x80000000, %%eax\n\t"
		"movl %%eax, %%cr0"
		: : "r"(kdir) : "eax", "memory");
	enabled = 1;
	klog(LOG_INFO, "paging: on, %u tables identity-map %u MiB, cr3=%x", tables, MAP_BYTES >> 20,
	     (uint32_t)kdir);
}

int paging_enabled(void)
{
	return enabled;
}

uint32_t paging_kernel_dir(void)
{
	return (uint32_t)kdir;
}
