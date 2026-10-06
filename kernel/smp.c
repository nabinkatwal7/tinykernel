#include "smp.h"

#include "apic.h"
#include "gdt.h"
#include "idt.h"
#include "io.h"
#include "klog.h"
#include "kstring.h"
#include "paging.h"
#include "percpu.h"
#include "pmm.h"
#include "sched.h"

#define TRAMP_ADDR  0x8000u
#define TRAMP_VECTOR (TRAMP_ADDR >> 12)
#define DATA_OFF    0x100u
#define AP_STACK_PAGES 4

#define LAPIC_ICR_LOW  0x300
#define LAPIC_ICR_HIGH 0x310
#define ICR_PENDING    (1u << 12)

extern const uint8_t ap_tramp_start[], ap_tramp_end[];

struct handshake {
	volatile uint32_t cr3, stack, entry, started;
};

static volatile uint32_t heartbeat[MAX_CPUS];
static struct {
	void (*volatile fn)(void *);
	void *volatile arg;
	volatile int done;
} job[MAX_CPUS];

int smp_post(int id, void (*fn)(void *), void *arg)
{
	struct percpu *c = percpu_get(id);

	if (!c || !c->online || c->is_bsp)
		return -1;
	job[id].done = 0;
	job[id].arg = arg;
	__asm__ volatile ("" : : : "memory");
	job[id].fn = fn;                /* publishing the function last starts the job */
	return 0;
}

int smp_wait(int id)
{
	int spin;

	for (spin = 0; !job[id].done && spin < 5000; spin++)
		task_sleep(1);
	return job[id].done ? 0 : -1;
}

int smp_run_on(int id, void (*fn)(void *), void *arg)
{
	return smp_post(id, fn, arg) ? -1 : smp_wait(id);
}
static volatile int ap_ready;      /* set by the AP once it is fully in the kernel */
static volatile int booting_cpu;   /* which logical cpu the AP currently starting is */

uint32_t smp_heartbeat(int id)
{
	return id >= 0 && id < MAX_CPUS ? heartbeat[id] : 0;
}

static void icr_send(uint32_t apic_id, uint32_t low)
{
	apic_write(LAPIC_ICR_HIGH, apic_id << 24);
	apic_write(LAPIC_ICR_LOW, low);
	while (apic_read(LAPIC_ICR_LOW) & ICR_PENDING)
		;
}

/* First C code on an application processor. Everything is still single-threaded on its own stack. */
static void ap_main(void)
{
	struct percpu *c;

	gdt_load_current();           /* the kernel's GDT (the trampoline used a private one) */
	idt_load();
	apic_write(0x0F0, 0x100 | 0xFF);   /* enable this core's local APIC */
	c = percpu_by_apic(apic_id());
	if (c) {
		c->online = 1;
		booting_cpu = c->id;
	}
	ap_ready = 1;
	for (;;) { /* idle with interrupts off: no scheduler on this core yet */
		heartbeat[booting_cpu]++;
		if (job[booting_cpu].fn) {
			void (*fn)(void *) = job[booting_cpu].fn;

			job[booting_cpu].fn = 0;
			fn(job[booting_cpu].arg);
			job[booting_cpu].done = 1;
		}
		__asm__ volatile ("pause");
	}
}

int smp_start_cpu(int id)
{
	struct percpu *c = percpu_get(id);
	struct handshake *h = (struct handshake *)(TRAMP_ADDR + DATA_OFF);
	uint32_t stack, i;

	if (!c || !c->present || c->online || c->is_bsp || !apic_present())
		return -1;
	stack = pmm_alloc_contig(AP_STACK_PAGES);
	if (!stack)
		return -1;
	memcpy((void *)TRAMP_ADDR, ap_tramp_start, (size_t)(ap_tramp_end - ap_tramp_start));
	h->cr3 = paging_kernel_dir();
	h->stack = stack + AP_STACK_PAGES * PAGE_SIZE - 16;
	h->entry = (uint32_t)ap_main;
	h->started = 0;
	ap_ready = 0;
	booting_cpu = id;
	c->stack_top = h->stack;

	icr_send(c->apic_id, 0x00004500);         /* INIT, level assert */
	task_sleep(10);
	for (i = 0; i < 2; i++) {                  /* STARTUP IPI, twice as the protocol asks */
		icr_send(c->apic_id, 0x00004600 | TRAMP_VECTOR);
		task_sleep(2);
	}
	for (i = 0; i < 100 && !ap_ready; i++)
		task_sleep(10);
	if (!ap_ready) {
		klog(LOG_WARN, "smp: cpu %d (apic %u) did not come up (trampoline reached: %u)", id, c->apic_id, h->started);
		return -1;
	}
	klog(LOG_INFO, "smp: cpu %d (apic %u) is online", id, c->apic_id);
	return 0;
}

int smp_start_all(void)
{
	int i, up = 0;

	for (i = 0; i < percpu_count(); i++)
		if (!smp_start_cpu(i))
			up++;
	return up;
}
