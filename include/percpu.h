#ifndef PERCPU_H
#define PERCPU_H

#include <stdint.h>

#define MAX_CPUS 8

/* Everything that belongs to one processor core. */
struct percpu {
	int id;                    /* logical number: 0 = the boot processor */
	uint32_t apic_id;          /* hardware id, as read from the local APIC */
	int present;               /* listed by ACPI (and enabled) */
	int online;                /* running our code */
	int is_bsp;
	uint32_t timer_irqs;       /* timer interrupts this core has taken */
	uint32_t interrupts;       /* all interrupts */
	void *current_task;        /* what this core is running (filled in by the scheduler) */
	void *idle_task;
	uint32_t stack_top;        /* its kernel stack */
};

void percpu_init(void);                       /* build the table from ACPI; the caller is the boot CPU */
struct percpu *this_cpu(void);                /* the core executing this call */
struct percpu *percpu_get(int id);
int  percpu_count(void);                      /* cores listed (present) */
int  percpu_online_count(void);
struct percpu *percpu_by_apic(uint32_t apic_id);

#endif
