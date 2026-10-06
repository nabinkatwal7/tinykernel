#include "idt.h"

#include "console.h"
#include "debug.h"
#include "gdt.h"
#include "apic.h"
#include "klog.h"
#include "paging.h"
#include "sched.h"
#include "pic.h"
#include "syscall.h"
#include "user.h"

struct idt_entry {
	uint16_t base_lo;
	uint16_t sel;
	uint8_t zero;
	uint8_t flags;
	uint16_t base_hi;
} __attribute__((packed));

struct idt_ptr {
	uint16_t limit;
	uint32_t base;
} __attribute__((packed));

extern uint32_t isr_stub_table[48];
extern void isr128(void);
extern void isr255(void);

static struct idt_entry idt[256];
static struct idt_ptr ip;
static irq_handler_t handlers[16];

static const char *const exc_names[32] = {
	"divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
	"invalid opcode", "device not available", "double fault", "coprocessor overrun",
	"invalid TSS", "segment not present", "stack fault", "general protection",
	"page fault", "reserved", "x87 FPU error", "alignment check", "machine check",
	"SIMD FPU error", "virtualization", "reserved", "reserved", "reserved", "reserved",
	"reserved", "reserved", "reserved", "reserved", "reserved", "security", "reserved",
};

static void set_gate(int i, uint32_t handler, uint8_t flags)
{
	idt[i].base_lo = (uint16_t)(handler & 0xFFFF);
	idt[i].sel = SEL_KCODE;
	idt[i].zero = 0;
	idt[i].flags = flags;
	idt[i].base_hi = (uint16_t)(handler >> 16);
}

void idt_init(void)
{
	int i;

	for (i = 0; i < 48; i++)
		set_gate(i, isr_stub_table[i], 0x8E); /* present, ring0, 32-bit interrupt gate */
	/* Double fault: a task gate, so the handler gets a fresh stack even if ours is gone. */
	idt[8].base_lo = 0;
	idt[8].sel = SEL_DF;
	idt[8].zero = 0;
	idt[8].flags = 0x85;
	idt[8].base_hi = 0;
	set_gate(0xFF, (uint32_t)isr255, 0x8E);       /* local APIC spurious interrupt: just return */
	set_gate(0x80, (uint32_t)isr128, 0xEE);       /* same, but callable from ring 3 */

	ip.limit = sizeof idt - 1;
	ip.base = (uint32_t)idt;
	__asm__ volatile ("lidt %0" : : "m"(ip));
}

void double_fault_task(void)
{
	uint32_t cr2;
	const char *owner;

	__asm__ volatile ("movl %%cr2, %0" : "=r"(cr2));
	owner = sched_guard_owner(cr2);
	console_set_color(COLOR_WHITE, COLOR_RED);
	if (owner)
		console_printf("\n*** KERNEL STACK OVERFLOW in task '%s' (guard page %08x) ***\n", owner, cr2);
	else
		console_printf("\n*** DOUBLE FAULT (cr2=%08x) ***\n", cr2);
	klog(LOG_ERROR, "double fault cr2=%x owner=%s", cr2, owner ? owner : "?");
	for (;;)
		__asm__ volatile ("cli; hlt");
}

void irq_install_handler(int irq, irq_handler_t h)
{
	handlers[irq] = h;
}

static void exception(struct regs *r)
{
	const char *name = exc_names[r->int_no];

	if (r->int_no == 14)
		paging_fault(r);

	if (r->cs & 3) { /* fault in a user program: kill it, keep the kernel alive */
		console_printf("\n[user fault] %s at eip=%08x (err=%x)\n", name, r->eip, r->err_code);
		klog(LOG_WARN, "user fault: %s eip=%x", name, r->eip);
		user_abort();
	}

	console_set_color(COLOR_WHITE, COLOR_RED);
	console_printf("\n*** EXCEPTION: %s ***\n", name);
	debug_dump_regs(r);
	panic("unhandled CPU exception %u (%s)", r->int_no, name);
}

void interrupt_dispatch(struct regs *r)
{
	int irq;

	if (r->int_no == 0x80) {
		syscall_dispatch(r);
		return;
	}
	if (r->int_no < 32) {
		exception(r);
		return;
	}

	irq = (int)r->int_no - 32;
	if (pic_is_spurious(irq))
		return;
	if (irq == 0 && apic_timer_active())
		apic_eoi();
	else
		pic_eoi(irq); /* before the handler: it may context-switch away */
	if (handlers[irq])
		handlers[irq](r);
}
