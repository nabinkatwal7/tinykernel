#include "percpu.h"

#include "acpi.h"
#include "apic.h"
#include "klog.h"
#include "kstring.h"

static struct percpu cpus[MAX_CPUS];
static int ncpus;

void percpu_init(void)
{
	const struct acpi_madt *m = acpi_madt();
	uint32_t boot_apic = apic_present() ? apic_id() : 0;
	int i;

	memset(cpus, 0, sizeof cpus);
	ncpus = 0;
	if (m->valid) {
		for (i = 0; i < m->ncpus && ncpus < MAX_CPUS; i++) {
			if (!m->cpu_enabled[i])
				continue;
			cpus[ncpus].id = ncpus;
			cpus[ncpus].apic_id = m->cpu_apic_id[i];
			cpus[ncpus].present = 1;
			ncpus++;
		}
	}
	if (!ncpus) { /* no MADT: assume the single processor we are running on */
		cpus[0].present = 1;
		cpus[0].apic_id = boot_apic;
		ncpus = 1;
	}
	for (i = 0; i < ncpus; i++) {
		if (cpus[i].apic_id == boot_apic) {
			cpus[i].is_bsp = 1;
			cpus[i].online = 1;
			cpus[i].stack_top = 0x90000; /* the boot stack */
		}
	}
	klog(LOG_INFO, "percpu: %d core(s), boot processor has apic id %u", ncpus, boot_apic);
}

struct percpu *percpu_by_apic(uint32_t apic)
{
	int i;

	for (i = 0; i < ncpus; i++)
		if (cpus[i].apic_id == apic)
			return &cpus[i];
	return 0;
}

struct percpu *this_cpu(void)
{
	struct percpu *c = apic_present() ? percpu_by_apic(apic_id()) : 0;

	return c ? c : &cpus[0];
}

struct percpu *percpu_get(int id)
{
	return id >= 0 && id < ncpus ? &cpus[id] : 0;
}

int percpu_count(void)
{
	return ncpus;
}

int percpu_online_count(void)
{
	int i, n = 0;

	for (i = 0; i < ncpus; i++)
		n += cpus[i].online;
	return n;
}
