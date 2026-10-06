#ifndef PCI_H
#define PCI_H

#include <stdint.h>

struct pci_dev {
	uint8_t bus, slot, func;
	uint16_t vendor, device;
	uint8_t class_code, subclass, prog_if, revision;
	uint8_t header_type;
	uint8_t irq_line;
	uint32_t bar[6];
};

#define PCI_MAX_DEVICES 32

/* Configuration space access (mechanism #1, ports 0xCF8/0xCFC). */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t  pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);

int      pci_scan(void);                       /* enumerate every bus; returns the device count */
int      pci_count(void);
const struct pci_dev *pci_get(int index);
const struct pci_dev *pci_find(uint16_t vendor, uint16_t device);  /* NULL if absent */

const char *pci_class_name(uint8_t class_code, uint8_t subclass);
const char *pci_vendor_name(uint16_t vendor);

#endif
