#ifndef APIC_H
#define APIC_H

#include <stdint.h>

/* Local APIC and I/O APIC discovery (the timer and interrupt routing come in later steps). */
int      apic_init(void);              /* 0 if a local APIC is present, enabled and mapped */
int      apic_present(void);
uint32_t apic_base(void);
uint32_t apic_id(void);                /* of the CPU we run on */
uint32_t apic_version(void);
uint32_t apic_read(uint32_t reg);
void     apic_write(uint32_t reg, uint32_t value);

int      ioapic_count(void);
uint32_t ioapic_address(int index);
uint32_t ioapic_read(int index, uint32_t reg);
void     ioapic_write(int index, uint32_t reg, uint32_t value);
uint32_t ioapic_max_redirection(int index);   /* number of redirection entries - 1 */

#endif
