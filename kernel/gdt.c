#include "gdt.h"
#include "idt.h"

struct gdt_entry {
	uint16_t limit_lo;
	uint16_t base_lo;
	uint8_t base_mid;
	uint8_t access;
	uint8_t gran;
	uint8_t base_hi;
} __attribute__((packed));

struct gdt_ptr {
	uint16_t limit;
	uint32_t base;
} __attribute__((packed));

struct tss {
	uint32_t prev, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
	uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
	uint32_t es, cs, ss, ds, fs, gs, ldt;
	uint16_t trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[7];
static struct gdt_ptr gp;
static struct tss tss;
static struct tss df_tss;                 /* hardware task switched to on a double fault */
static uint8_t df_stack[4096] __attribute__((aligned(16)));

static void set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
	gdt[i].limit_lo = (uint16_t)(limit & 0xFFFF);
	gdt[i].base_lo = (uint16_t)(base & 0xFFFF);
	gdt[i].base_mid = (uint8_t)((base >> 16) & 0xFF);
	gdt[i].access = access;
	gdt[i].gran = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
	gdt[i].base_hi = (uint8_t)(base >> 24);
}

void gdt_set_df_cr3(uint32_t cr3)
{
	df_tss.cr3 = cr3;
}

void gdt_set_kernel_stack(uint32_t esp0)
{
	tss.esp0 = esp0;
}

void gdt_init(void)
{
	set_entry(0, 0, 0, 0, 0);
	set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF); /* kernel code */
	set_entry(2, 0, 0xFFFFF, 0x92, 0xCF); /* kernel data */
	set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF); /* user code   */
	set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF); /* user data   */
	set_entry(5, (uint32_t)&tss, sizeof tss - 1, 0x89, 0x00);
	set_entry(6, (uint32_t)&df_tss, sizeof df_tss - 1, 0x89, 0x00);

	df_tss.eip = (uint32_t)double_fault_task;
	df_tss.eflags = 2;
	df_tss.esp = (uint32_t)df_stack + sizeof df_stack - 16;
	df_tss.cs = SEL_KCODE;
	df_tss.ss = df_tss.ds = df_tss.es = df_tss.fs = df_tss.gs = SEL_KDATA;
	df_tss.iomap_base = sizeof df_tss;

	tss.ss0 = SEL_KDATA;
	tss.iomap_base = sizeof tss;

	gp.limit = sizeof gdt - 1;
	gp.base = (uint32_t)gdt;

	__asm__ volatile (
		"lgdt %0\n\t"
		"movw $0x10, %%ax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%fs\n\t"
		"movw %%ax, %%gs\n\t"
		"movw %%ax, %%ss\n\t"
		"ljmp $0x08, $1f\n"
		"1:\n\t"
		"movw $0x28, %%ax\n\t"
		"ltr %%ax"
		: : "m"(gp) : "ax", "memory");
}
