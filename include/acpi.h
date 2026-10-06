#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/*
 * Just enough ACPI for an orderly power-off: locate the RSDP, walk the RSDT to the FADT, read the
 * PM1a control port and SMI command port, and dig the \_S5 sleep type out of the DSDT.
 */
int  acpi_init(void);                    /* 0 if the tables were found and understood */
int  acpi_available(void);
void acpi_poweroff(void);                /* does not return if it works */

struct acpi_table_info {
	char signature[5];
	uint32_t address;
	uint32_t length;
};
/* Parsed MADT (the "APIC" table): processors, I/O APICs and interrupt overrides. */
#define ACPI_MAX_CPUS 8
#define ACPI_MAX_IOAPICS 2
#define ACPI_MAX_OVERRIDES 8
struct acpi_madt {
	int valid;
	uint32_t lapic_addr;
	int ncpus;
	uint8_t cpu_apic_id[ACPI_MAX_CPUS];
	uint8_t cpu_enabled[ACPI_MAX_CPUS];
	int nioapics;
	uint8_t ioapic_id[ACPI_MAX_IOAPICS];
	uint32_t ioapic_addr[ACPI_MAX_IOAPICS];
	uint32_t ioapic_gsi_base[ACPI_MAX_IOAPICS];
	int noverrides;
	uint8_t override_source[ACPI_MAX_OVERRIDES];
	uint32_t override_gsi[ACPI_MAX_OVERRIDES];
};
const struct acpi_madt *acpi_madt(void);

int  acpi_table_count(void);
int  acpi_table_get(int index, struct acpi_table_info *out);
void acpi_describe(uint8_t *revision, char oem[7], uint32_t *pm1a_cnt, uint32_t *smi_cmd, int *slp_typ, int *enabled);

#endif
