#include "apic.h"

#include "acpi.h"
#include "cpu.h"
#include "klog.h"
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
