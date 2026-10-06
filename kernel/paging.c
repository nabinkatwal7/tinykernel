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
		/* Same table again at KERNEL_VMA: the kernel image and a direct map of RAM. */
		kdir[(KERNEL_VMA + addr) >> 22] = (uint32_t)t | PTE_P | PTE_RW | PTE_US;
		tables++;
	}

	__asm__ volatile (
		"movl %0, %%cr3\n\t"
		"movl %%cr0, %%eax\n\t"
		"orl $0x80000000, %%eax\n\t"
		"movl %%eax, %%cr0"
		: : "r"(kdir) : "eax", "memory");
	enabled = 1;
	klog(LOG_INFO, "paging: on, %u tables map %u MiB at 0 and at 0xC0000000, cr3=%x", tables, MAP_BYTES >> 20,
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

static uint32_t current_dir(void)
{
	uint32_t cr3;

	__asm__ volatile ("movl %%cr3, %0" : "=r"(cr3));
	return cr3;
}

static void flush_page(uint32_t virt)
{
	__asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

int paging_map(uint32_t dir, uint32_t virt, uint32_t phys, uint32_t flags)
{
	uint32_t *pd, *pt;
	uint32_t *pde;

	if (!dir)
		dir = current_dir();
	pd = (uint32_t *)dir;
	pde = &pd[virt >> 22];
	if (!(*pde & PTE_P)) {
		pt = alloc_table();
		if (!pt)
			return -1;
		*pde = (uint32_t)pt | PTE_P | PTE_RW | PTE_US;
	}
	pt = (uint32_t *)(*pde & ~0xFFFu);
	pt[(virt >> 12) & 0x3FF] = (phys & ~0xFFFu) | (flags & 0xFFFu) | PTE_P;
	if (dir == current_dir())
		flush_page(virt);
	return 0;
}

int paging_unmap(uint32_t dir, uint32_t virt)
{
	uint32_t *pd, *pt;

	if (!dir)
		dir = current_dir();
	pd = (uint32_t *)dir;
	if (!(pd[virt >> 22] & PTE_P))
		return -1;
	pt = (uint32_t *)(pd[virt >> 22] & ~0xFFFu);
	if (!(pt[(virt >> 12) & 0x3FF] & PTE_P))
		return -1;
	pt[(virt >> 12) & 0x3FF] = 0;
	if (dir == current_dir())
		flush_page(virt);
	return 0;
}

uint32_t paging_translate(uint32_t dir, uint32_t virt)
{
	uint32_t *pd, *pt, pte;

	if (!dir)
		dir = current_dir();
	pd = (uint32_t *)dir;
	if (!(pd[virt >> 22] & PTE_P))
		return PAGING_NOT_MAPPED;
	pt = (uint32_t *)(pd[virt >> 22] & ~0xFFFu);
	pte = pt[(virt >> 12) & 0x3FF];
	if (!(pte & PTE_P))
		return PAGING_NOT_MAPPED;
	return (pte & ~0xFFFu) | (virt & 0xFFFu);
}

int paging_is_mapped(uint32_t virt)
{
	return !enabled || paging_translate(0, virt) != PAGING_NOT_MAPPED;
}
