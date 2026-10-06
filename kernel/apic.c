#include "apic.h"

#include "acpi.h"
#include "cpu.h"
#include "idt.h"
#include "io.h"
#include "klog.h"
#include "pic.h"
#include "timer.h"
#include "paging.h"

#define MSR_APIC_BASE 0x1B
#define LAPIC_ID      0x020
#define LAPIC_VERSION 0x030

static uint32_t lapic;       /* virtual (= physical) base address */
static int present;

static uint64_t rdmsr(uint32_t msr)
{
	uint32_t lo, hi;

	__asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

/* Map one device page (uncached) into the kernel address space, identity style. */
static int map_device(uint32_t phys)
{
	if (paging_is_mapped(phys))
		return 0;
	return paging_map(0, phys, phys, PTE_RW | PTE_PCD);
}

int apic_init(void)
{
	const struct acpi_madt *m = acpi_madt();
	uint32_t base;
	int i;

	present = 0;
	if (!(cpu_get()->features_edx & CPU_APIC)) {
		klog(LOG_INFO, "apic: CPU has no local APIC");
		return -1;
	}
	base = (uint32_t)rdmsr(MSR_APIC_BASE) & 0xFFFFF000u;
	if (m->valid && m->lapic_addr)
		base = m->lapic_addr;
	if (map_device(base)) {
		klog(LOG_WARN, "apic: cannot map the local APIC at %x", base);
		return -1;
	}
	lapic = base;
	for (i = 0; i < m->nioapics; i++)
		map_device(m->ioapic_addr[i]);
	present = 1;
	klog(LOG_INFO, "apic: local APIC at %x id %u version %x, %d I/O APIC(s)", lapic, apic_id(),
	     apic_version() & 0xFF, m->nioapics);
	return 0;
}

#define LAPIC_EOI       0x0B0
#define LAPIC_SPURIOUS  0x0F0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0
#define TIMER_VECTOR    0x20  /* the vector IRQ0 used, so the existing timer handler keeps working */
#define SPURIOUS_VECTOR 0xFF

static int timer_on;
static uint32_t counts_per_ms;

void apic_timer_start_local(void)
{
	apic_write(LAPIC_TIMER_DIV, 0x3);
	apic_write(LAPIC_LVT_TIMER, (1u << 17) | TIMER_VECTOR);   /* periodic, same vector as the boot core */
	apic_write(LAPIC_TIMER_INIT, counts_per_ms * 10);          /* 100 Hz */
}

void apic_eoi(void)
{
	apic_write(LAPIC_EOI, 0);
}

int apic_timer_active(void) { return timer_on; }
uint32_t apic_timer_ticks_per_ms(void) { return counts_per_ms; }

int apic_timer_start(uint32_t hz)
{
	uint32_t start, counted;

	if (!present || !hz || timer_on)
		return -1;
	apic_write(LAPIC_SPURIOUS, 0x100 | SPURIOUS_VECTOR); /* software-enable the local APIC */
	apic_write(LAPIC_TIMER_DIV, 0x3);                    /* divide the bus clock by 16 */

	/* calibrate: let it count down freely while 10 PIT ticks (100 ms at 100 Hz) go by */
	apic_write(LAPIC_LVT_TIMER, 1u << 16 | TIMER_VECTOR); /* masked one-shot: counts without interrupting */
	start = timer_ticks();
	while (timer_ticks() == start)
		;
	start = timer_ticks();
	apic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
	while (timer_ticks() - start < 10)
		;
	counted = 0xFFFFFFFFu - apic_read(LAPIC_TIMER_CUR);
	counts_per_ms = counted / (10u * 1000u / timer_hz());
	if (!counts_per_ms) {
		klog(LOG_WARN, "apic: timer calibration failed");
		return -1;
	}

	{
		uint32_t flags = irq_save();

		pic_mask(0);                                   /* the PIT stops ticking us ... */
		apic_write(LAPIC_LVT_TIMER, (1u << 17) | TIMER_VECTOR); /* ... the LAPIC takes over: periodic */
		apic_write(LAPIC_TIMER_INIT, counts_per_ms * (1000u / hz));
		timer_on = 1;
		irq_restore(flags);
	}
	klog(LOG_INFO, "apic: timer %u counts/ms, ticking at %u Hz (PIT masked)", counts_per_ms, hz);
	return 0;
}

int apic_present(void) { return present; }
uint32_t apic_base(void) { return lapic; }

uint32_t apic_read(uint32_t reg)
{
	return *(volatile uint32_t *)(lapic + reg);
}

void apic_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(lapic + reg) = value;
}

uint32_t apic_id(void) { return present ? apic_read(LAPIC_ID) >> 24 : 0; }
uint32_t apic_version(void) { return present ? apic_read(LAPIC_VERSION) : 0; }

int ioapic_count(void) { return acpi_madt()->nioapics; }

uint32_t ioapic_address(int i)
{
	return i >= 0 && i < ioapic_count() ? acpi_madt()->ioapic_addr[i] : 0;
}

/* Indirect access: write the register number to IOREGSEL (+0), then read/write IOWIN (+0x10). */
uint32_t ioapic_read(int i, uint32_t reg)
{
	uint32_t a = ioapic_address(i);

	if (!a)
		return 0;
	*(volatile uint32_t *)a = reg;
	return *(volatile uint32_t *)(a + 0x10);
}

void ioapic_write(int i, uint32_t reg, uint32_t value)
{
	uint32_t a = ioapic_address(i);

	if (!a)
		return;
	*(volatile uint32_t *)a = reg;
	*(volatile uint32_t *)(a + 0x10) = value;
}

uint32_t ioapic_max_redirection(int i)
{
	return (ioapic_read(i, 1) >> 16) & 0xFF;
}
