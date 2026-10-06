#include "paging.h"

#include "console.h"
#include "debug.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "klog.h"
#include "kstring.h"
#include "pmm.h"
#include "sched.h"
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
		"orl $0x80010000, %%eax\n\t"   /* PG + WP: the kernel obeys read-only pages too */
		"movl %%eax, %%cr0"
		: : "r"(kdir) : "eax", "memory");
	enabled = 1;
	gdt_set_df_cr3((uint32_t)kdir);
	klog(LOG_INFO, "paging: on, %u tables map %u MiB at 0 and at 0xC0000000, cr3=%x", tables, MAP_BYTES >> 20,
	     (uint32_t)kdir);
}

void paging_fault(struct regs *r)
{
	uint32_t addr;
	uint32_t cr2;

	__asm__ volatile ("movl %%cr2, %0" : "=r"(cr2));
	if (paging_cow_fault(cr2, r->err_code))
		return;
	if (!(r->err_code & 1) && user_demand_fault(cr2))
		return; /* a not-present stack page was just created; the instruction retries */ /* a write to a shared page: it now has its own copy and the instruction retries */
	const char *kind = !(r->err_code & 1) ? "page not present"
		: (r->err_code & 2) ? "write to read-only page" : "protection violation";

	__asm__ volatile ("movl %%cr2, %0" : "=r"(addr));
	if (r->cs & 3) {
		console_printf("\n[user fault] page fault: %s at %08x (eip=%08x)\n", kind, addr,
			       r->eip);
		klog(LOG_WARN, "user page fault: %s addr=%x eip=%x", kind, addr, r->eip);
		user_abort();
	}
	{
		const char *owner = sched_guard_owner(addr);

		if (owner)
			panic("kernel stack overflow in task %s (guard page %x hit)", owner, addr);
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

/* Make sure 'dir' owns the page table behind 'pde' (clone it if it is shared with the kernel). */
static uint32_t *own_table(uint32_t *pd, uint32_t virt)
{
	uint32_t *pde = &pd[virt >> 22];
	uint32_t *pt;

	if (!(*pde & PTE_P)) {
		pt = alloc_table();
		if (!pt)
			return 0;
		*pde = (uint32_t)pt | PTE_P | PTE_RW | PTE_US;
	} else if (pd != kdir && (*pde & ~0xFFFu) == (kdir[virt >> 22] & ~0xFFFu)) {
		pt = alloc_table();
		if (!pt)
			return 0;
		memcpy(pt, (void *)(*pde & ~0xFFFu), PAGE_SIZE);
		*pde = (uint32_t)pt | (*pde & 0xFFFu);
	}
	return (uint32_t *)(*pde & ~0xFFFu);
}

int paging_map(uint32_t dir, uint32_t virt, uint32_t phys, uint32_t flags)
{
	uint32_t *pt;

	if (!dir)
		dir = current_dir();
	pt = own_table((uint32_t *)dir, virt);
	if (!pt)
		return -1;
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
	pt = own_table(pd, virt);
	if (!pt || !(pt[(virt >> 12) & 0x3FF] & PTE_P))
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

/*
 * A new address space starts as a copy of the kernel directory: the page tables themselves are
 * shared, so kernel mappings stay identical everywhere. Mapping something new in a fresh
 * directory (paging_map) allocates a private table only for that 4 MiB slot, so it never
 * shows up in other spaces. Shared kernel tables are recognised by pointer equality.
 */
uint32_t paging_new_dir(void)
{
	uint32_t *d = alloc_table();

	if (!d)
		return 0;
	memcpy(d, kdir, PAGE_SIZE);
	return (uint32_t)d;
}

void paging_free_dir(uint32_t dir)
{
	uint32_t *d = (uint32_t *)dir;
	uint32_t i;

	if (!dir || d == kdir)
		return;
	for (i = 0; i < ENTRIES; i++)
		if ((d[i] & PTE_P) && (d[i] & ~0xFFFu) != (kdir[i] & ~0xFFFu))
			pmm_free(d[i] & ~0xFFFu); /* private table */
	pmm_free(dir);
}

void paging_switch(uint32_t dir)
{
	if (enabled && dir && dir != current_dir())
		__asm__ volatile ("movl %0, %%cr3" : : "r"(dir) : "memory");
}

uint32_t *paging_pte(uint32_t dir, uint32_t virt)
{
	uint32_t *pd, *pt;

	if (!dir)
		dir = current_dir();
	pd = (uint32_t *)dir;
	if (!(pd[virt >> 22] & PTE_P))
		return 0;
	pt = (uint32_t *)(pd[virt >> 22] & ~0xFFFu);
	return &pt[(virt >> 12) & 0x3FF];
}

/*
 * Copy-on-write clone: kernel tables stay shared, every private table is duplicated, and each user
 * page in it becomes read-only + PTE_COW in BOTH spaces with its frame's refcount raised.
 */
uint32_t paging_fork_dir(uint32_t dir)
{
	uint32_t *src = (uint32_t *)(dir ? dir : current_dir());
	uint32_t *d = alloc_table();
	uint32_t i, j;
	uint32_t flags = irq_save();

	if (!d) {
		irq_restore(flags);
		return 0;
	}
	for (i = 0; i < ENTRIES; i++) {
		uint32_t *st, *dt;

		if (!(src[i] & PTE_P) || (src[i] & ~0xFFFu) == (kdir[i] & ~0xFFFu)) {
			d[i] = src[i];          /* absent, or a kernel table shared by everyone */
			continue;
		}
		dt = alloc_table();
		if (!dt) {
			irq_restore(flags);
			return 0;                 /* (the partial directory leaks: out of memory is fatal enough) */
		}
		st = (uint32_t *)(src[i] & ~0xFFFu);
		for (j = 0; j < ENTRIES; j++) {
			uint32_t e = st[j];

			if ((e & PTE_P) && (e & PTE_US)) {
				if (e & PTE_RW)
					e = (e & ~PTE_RW) | PTE_COW;     /* writable pages become copy-on-write */
				st[j] = e;                           /* the parent loses write access too */
				pmm_ref(e & ~0xFFFu);
			}
			dt[j] = e;
		}
		d[i] = (uint32_t)dt | (src[i] & 0xFFFu);
	}
	/* the parent's TLB may still hold the writable translations */
	if ((uint32_t)src == current_dir())
		__asm__ volatile ("movl %%cr3, %%eax\n\tmovl %%eax, %%cr3" : : : "eax", "memory");
	irq_restore(flags);
	return (uint32_t)d;
}

int paging_cow_fault(uint32_t addr, uint32_t err)
{
	uint32_t *pte, e, frame, copy;

	if (!(err & 1) || !(err & 2)) /* only "write to a present page" can be a COW fault */
		return 0;
	pte = paging_pte(0, addr);
	if (!pte || !(*pte & PTE_COW))
		return 0;
	e = *pte;
	frame = e & ~0xFFFu;
	if (pmm_refcount(frame) <= 1) {          /* last owner: no copy needed, just allow writing again */
		*pte = (e | PTE_RW) & ~PTE_COW;
	} else {
		copy = pmm_alloc();
		if (!copy)
			return 0;                    /* out of memory: the fault stays fatal */
		memcpy((void *)copy, (void *)frame, PAGE_SIZE);
		*pte = copy | ((e | PTE_RW) & 0xFFFu & ~PTE_COW);
		pmm_free(frame);                     /* drop our reference to the shared original */
	}
	flush_page(addr);
	return 1;
}

void paging_destroy_user(uint32_t dir)
{
	uint32_t *d = (uint32_t *)dir;
	uint32_t i, j;

	if (!dir || d == kdir)
		return;
	for (i = 0; i < ENTRIES; i++) {
		uint32_t *t;

		if (!(d[i] & PTE_P) || (d[i] & ~0xFFFu) == (kdir[i] & ~0xFFFu))
			continue;                      /* shared kernel table: not ours to free */
		t = (uint32_t *)(d[i] & ~0xFFFu);
		for (j = 0; j < ENTRIES; j++)
			if ((t[j] & PTE_P) && (t[j] & PTE_US))
				pmm_free(t[j] & ~0xFFFu);  /* drops our reference; the frame lives on if shared */
		pmm_free((uint32_t)t);
	}
	pmm_free(dir);
}
