#include "paging.h"

#include "console.h"
#include "debug.h"
#include "idt.h"
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

void paging_fault(struct regs *r)
{
	uint32_t addr;
	const char *kind = !(r->err_code & 1) ? "page not present"
		: (r->err_code & 2) ? "write to read-only page" : "protection violation";

	__asm__ volatile ("movl %%cr2, %0" : "=r"(addr));
	if (r->cs & 3) {
		console_printf("\n[user fault] page fault: %s at %08x (eip=%08x)\n", kind, addr,
			       r->eip);
		klog(LOG_WARN, "user page fault: %s addr=%x eip=%x", kind, addr, r->eip);
		user_abort();
	}
	console_set_color(COLOR_WHITE, COLOR_RED);
	console_printf("\n*** PAGE FAULT: %s at %08x ***\n", kind, addr);
	debug_dump_regs(r);
	panic("kernel page fault at %x (%s) eip=%x", addr, kind, r->eip);
}

int paging_enabled(void)
{
	return enabled;
}

uint32_t paging_kernel_dir(void)
{
	return (uint32_t)kdir;
}
