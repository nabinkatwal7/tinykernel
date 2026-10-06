#include "pmm.h"

#include "console.h"
#include "io.h"
#include "klog.h"

/* Bootloader leaves the BIOS memory map here: magic, count, then 24-byte entries. */
#define E820_BASE   0x500u
#define E820_MAGIC  0x30323845u
#define E820_USABLE 1u

#define MAX_FRAMES  16384u /* manage the first 64 MiB */
#define LOW_LIMIT   0x100000u /* never hand out the first MiB */

struct e820_entry {
	uint32_t base_lo, base_hi;
	uint32_t len_lo, len_hi;
	uint32_t type;
	uint32_t acpi;
};

extern char kernel_end[];

static uint32_t bitmap[MAX_FRAMES / 32]; /* 1 = used */
static uint32_t hint;
static uint32_t free_count;

static int test(uint32_t f) { return bitmap[f / 32] & (1u << (f % 32)); }
static void set(uint32_t f) { bitmap[f / 32] |= 1u << (f % 32); }
static void clear(uint32_t f) { bitmap[f / 32] &= ~(1u << (f % 32)); }

static void free_region(uint32_t start, uint32_t end)
{
	uint32_t f;

	if (end > MAX_FRAMES * PAGE_SIZE)
		end = MAX_FRAMES * PAGE_SIZE;
	for (f = (start + PAGE_SIZE - 1) / PAGE_SIZE; (f + 1) * PAGE_SIZE <= end; f++) {
		if (test(f)) {
			clear(f);
			free_count++;
		}
	}
}

static uint32_t e820_count(void)
{
	uint32_t *p = (uint32_t *)E820_BASE;

	if (p[0] != E820_MAGIC || p[1] == 0 || p[1] > 32)
		return 0;
	return p[1];
}

void pmm_init(void)
{
	struct e820_entry *e = (struct e820_entry *)(E820_BASE + 8);
	uint32_t n = e820_count(), i, f;
	uint32_t kend = (((uint32_t)kernel_end - KERNEL_VMA) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	for (f = 0; f < MAX_FRAMES / 32; f++)
		bitmap[f] = 0xFFFFFFFFu;
	free_count = 0;

	if (n == 0) {
		klog(LOG_WARN, "pmm: no E820 map, assuming 1-16 MiB usable");
		free_region(LOW_LIMIT, 16u << 20);
	}
	for (i = 0; i < n; i++) {
		uint32_t start, end;

		if (e[i].type != E820_USABLE || e[i].base_hi)
			continue;
		start = e[i].base_lo;
		end = e[i].len_hi ? 0xFFFFF000u : start + e[i].len_lo;
		if (end < start)
			end = 0xFFFFF000u;
		if (start < LOW_LIMIT)
			start = LOW_LIMIT;
		if (end > start)
			free_region(start, end);
	}

	/* The kernel image lives at 1 MiB. */
	for (f = LOW_LIMIT / PAGE_SIZE; f < kend / PAGE_SIZE; f++) {
		if (!test(f)) {
			set(f);
			free_count--;
		}
	}
	hint = kend / PAGE_SIZE;
	klog(LOG_INFO, "pmm: %u KiB free in %u frames, kernel ends at %x", free_count * 4,
	     free_count, kend);
}

uint32_t pmm_alloc_contig(uint32_t n)
{
	uint32_t f, run = 0, start = 0, tries;
	uint32_t flags = irq_save();

	if (n == 0)
		goto fail;
	for (tries = 0, f = hint; tries < MAX_FRAMES; tries++, f++) {
		if (f >= MAX_FRAMES) {
			f = 0;
			run = 0;
		}
		if (test(f)) {
			run = 0;
			continue;
		}
		if (run++ == 0)
			start = f;
		if (run == n) {
			uint32_t i;

			for (i = start; i < start + n; i++)
				set(i);
			free_count -= n;
			hint = start + n;
			irq_restore(flags);
			return start * PAGE_SIZE;
		}
	}
fail:
	irq_restore(flags);
	return 0;
}

uint32_t pmm_alloc(void)
{
	return pmm_alloc_contig(1);
}

int pmm_reserve(uint32_t addr, uint32_t n)
{
	uint32_t f = addr / PAGE_SIZE, i;
	uint32_t flags = irq_save();

	if (f + n > MAX_FRAMES)
		goto fail;
	for (i = 0; i < n; i++)
		if (test(f + i))
			goto fail;
	for (i = 0; i < n; i++)
		set(f + i);
	free_count -= n;
	irq_restore(flags);
	return 0;
fail:
	irq_restore(flags);
	return -1;
}

void pmm_free_range(uint32_t addr, uint32_t n)
{
	uint32_t f = addr / PAGE_SIZE, i;
	uint32_t flags = irq_save();

	for (i = 0; i < n && f + i < MAX_FRAMES; i++) {
		if (test(f + i)) {
			clear(f + i);
			free_count++;
		}
	}
	if (f < hint)
		hint = f;
	irq_restore(flags);
}

void pmm_free(uint32_t addr)
{
	pmm_free_range(addr, 1);
}

uint32_t pmm_total_frames(void)
{
	return MAX_FRAMES;
}

uint32_t pmm_free_frames(void)
{
	return free_count;
}

/* Highest end address (clipped to 4 GiB) and total size of E820 usable regions, in KiB. */
static void e820_totals(uint32_t *top_kib, uint32_t *sum_kib)
{
	struct e820_entry *e = (struct e820_entry *)(E820_BASE + 8);
	uint32_t n = e820_count(), i;

	*top_kib = *sum_kib = 0;
	for (i = 0; i < n; i++) {
		uint32_t start, len, end;

		if (e[i].type != E820_USABLE || e[i].base_hi)
			continue;
		start = e[i].base_lo;
		len = e[i].len_hi ? 0xFFFFFFFFu - start : e[i].len_lo;
		end = start + len < start ? 0xFFFFFFFFu : start + len;
		*sum_kib += len / 1024;
		if (end / 1024 > *top_kib)
			*top_kib = end / 1024;
	}
}

uint32_t pmm_ram_kib(void)
{
	uint32_t top, sum;

	e820_totals(&top, &sum);
	return top ? top : pmm_cmos_ram_kib();
}

uint32_t pmm_usable_kib(void)
{
	uint32_t top, sum;

	e820_totals(&top, &sum);
	return sum;
}

static uint8_t cmos(uint8_t reg)
{
	outb(0x70, reg);
	return inb(0x71);
}

/* CMOS 0x30/0x31: KiB between 1 MiB and 16 MiB; 0x34/0x35: 64 KiB units above 16 MiB. */
uint32_t pmm_cmos_ram_kib(void)
{
	uint32_t low = (uint32_t)cmos(0x30) | ((uint32_t)cmos(0x31) << 8);
	uint32_t high = (uint32_t)cmos(0x34) | ((uint32_t)cmos(0x35) << 8);

	return high ? 16u * 1024 + high * 64 : 1024 + low;
}

void pmm_print_map(void)
{
	struct e820_entry *e = (struct e820_entry *)(E820_BASE + 8);
	uint32_t n = e820_count(), i;
	static const char *const types[] = { "?", "usable", "reserved", "ACPI reclaim", "ACPI NVS",
					     "bad" };

	if (n == 0) {
		console_write("E820 map: not available (fallback layout)\n");
	} else {
		console_write("E820 physical memory map:\n");
		for (i = 0; i < n; i++) {
			uint32_t t = e[i].type < 6 ? e[i].type : 0;

			console_printf("  %08x - %08x  %s\n", e[i].base_lo,
				       e[i].base_lo + e[i].len_lo - 1, types[t]);
		}
	}
	console_printf("RAM: %u MiB installed (E820), %u MiB usable, CMOS reports %u MiB\n",
		       (pmm_ram_kib() + 512) / 1024, pmm_usable_kib() / 1024,
		       (pmm_cmos_ram_kib() + 512) / 1024);
	console_printf("pmm: %u / %u frames free (%u KiB), managing first %u MiB\n", free_count,
		       MAX_FRAMES, free_count * 4, MAX_FRAMES * 4 / 1024);
}
