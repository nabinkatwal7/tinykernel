#include "pci.h"

#include "io.h"
#include "klog.h"
#include "kstring.h"

#define CONFIG_ADDRESS 0xCF8
#define CONFIG_DATA    0xCFC

static struct pci_dev devs[PCI_MAX_DEVICES];
static int ndevs;

static uint32_t address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
	return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)(slot & 31) << 11)
	       | ((uint32_t)(func & 7) << 8) | (off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
	outl(CONFIG_ADDRESS, address(bus, slot, func, off));
	return inl(CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
	return (uint16_t)(pci_read32(bus, slot, func, off) >> ((off & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
	return (uint8_t)(pci_read32(bus, slot, func, off) >> ((off & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
	outl(CONFIG_ADDRESS, address(bus, slot, func, off));
	outl(CONFIG_DATA, v);
}

static void add_function(uint8_t bus, uint8_t slot, uint8_t func)
{
	struct pci_dev *d;
	uint32_t id = pci_read32(bus, slot, func, 0x00), cls;
	int i;

	if ((id & 0xFFFF) == 0xFFFF || ndevs >= PCI_MAX_DEVICES)
		return;
	d = &devs[ndevs++];
	d->bus = bus;
	d->slot = slot;
	d->func = func;
	d->vendor = (uint16_t)id;
	d->device = (uint16_t)(id >> 16);
	cls = pci_read32(bus, slot, func, 0x08);
	d->revision = (uint8_t)cls;
	d->prog_if = (uint8_t)(cls >> 8);
	d->subclass = (uint8_t)(cls >> 16);
	d->class_code = (uint8_t)(cls >> 24);
	d->header_type = pci_read8(bus, slot, func, 0x0E);
	d->irq_line = pci_read8(bus, slot, func, 0x3C);
	for (i = 0; i < 6; i++)
		d->bar[i] = (d->header_type & 0x7F) == 0 ? pci_read32(bus, slot, func, (uint8_t)(0x10 + 4 * i)) : 0;
}

int pci_scan(void)
{
	int bus, slot, func;

	ndevs = 0;
	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			if ((pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0) & 0xFFFF) == 0xFFFF)
				continue; /* empty slot */
			add_function((uint8_t)bus, (uint8_t)slot, 0);
			if (pci_read8((uint8_t)bus, (uint8_t)slot, 0, 0x0E) & 0x80) /* multi-function */
				for (func = 1; func < 8; func++)
					add_function((uint8_t)bus, (uint8_t)slot, (uint8_t)func);
		}
	}
	for (bus = 0; bus < ndevs; bus++)
		klog(LOG_INFO, "pci: %02x:%02x.%u %04x:%04x class %02x%02x", devs[bus].bus, devs[bus].slot,
		     devs[bus].func, devs[bus].vendor, devs[bus].device, devs[bus].class_code,
		     devs[bus].subclass);
	klog(LOG_INFO, "pci: %d device(s)", ndevs);
	return ndevs;
}

int pci_count(void)
{
	return ndevs;
}

const struct pci_dev *pci_get(int i)
{
	return i >= 0 && i < ndevs ? &devs[i] : 0;
}

const struct pci_dev *pci_find(uint16_t vendor, uint16_t device)
{
	int i;

	for (i = 0; i < ndevs; i++)
		if (devs[i].vendor == vendor && devs[i].device == device)
			return &devs[i];
	return 0;
}

const char *pci_class_name(uint8_t c, uint8_t sub)
{
	switch (c) {
	case 0x00: return "Unclassified device";
	case 0x01:
		switch (sub) {
		case 0x00: return "SCSI controller";
		case 0x01: return "IDE interface";
		case 0x02: return "Floppy controller";
		case 0x06: return "SATA controller";
		}
		return "Mass storage controller";
	case 0x02: return sub == 0 ? "Ethernet controller" : "Network controller";
	case 0x03: return sub == 0 ? "VGA compatible controller" : "Display controller";
	case 0x04: return "Multimedia controller";
	case 0x05: return "Memory controller";
	case 0x06:
		switch (sub) {
		case 0x00: return "Host bridge";
		case 0x01: return "ISA bridge";
		case 0x04: return "PCI bridge";
		}
		return "Bridge";
	case 0x07: return "Communication controller";
	case 0x08: return "System peripheral";
	case 0x09: return "Input device controller";
	case 0x0C: return sub == 0x03 ? "USB controller" : "Serial bus controller";
	}
	return "Unknown device";
}

const char *pci_vendor_name(uint16_t v)
{
	switch (v) {
	case 0x8086: return "Intel";
	case 0x1234: return "QEMU/Bochs";
	case 0x10EC: return "Realtek";
	case 0x1AF4: return "Red Hat (virtio)";
	case 0x1022: return "AMD";
	case 0x10DE: return "NVIDIA";
	case 0x1013: return "Cirrus Logic";
	}
	return "Unknown vendor";
}
