#include "usb.h"

#include "console.h"
#include "io.h"
#include "paging.h"
#include "pci.h"

/*
 * USB host controller detection. USB controllers are PCI class 0C, subclass 03; the programming interface tells
 * the generation:  00 UHCI (USB 1.1, I/O ports), 10 OHCI (USB 1.1, memory), 20 EHCI (USB 2.0), 30 xHCI (USB 3.x).
 * For each one the command prints where its registers are and, where that is cheap, the number of root ports.
 */

static const char *kind_name(uint8_t prog_if)
{
	switch (prog_if) {
	case 0x00: return "UHCI (USB 1.1)";
	case 0x10: return "OHCI (USB 1.1)";
	case 0x20: return "EHCI (USB 2.0)";
	case 0x30: return "xHCI (USB 3.x)";
	case 0x80: return "unspecified";
	case 0xFE: return "device (not a host)";
	default: return "unknown";
	}
}

static int map(uint32_t base, uint32_t bytes)
{
	uint32_t off;

	for (off = 0; off < bytes; off += 4096)
		if (paging_map(0, (base & ~0xFFFu) + off, (base & ~0xFFFu) + off, PTE_RW | PTE_PCD))
			return -1;
	return 0;
}

int usb_count_controllers(void)
{
	int i, n = 0;

	for (i = 0; i < pci_count(); i++) {
		const struct pci_dev *d = pci_get(i);

		n += d->class_code == 0x0C && d->subclass == 0x03;
	}
	return n;
}

int cmd_usb(int argc, char **argv)
{
	int i, n = 0;

	(void)argc;
	(void)argv;
	for (i = 0; i < pci_count(); i++) {
		const struct pci_dev *d = pci_get(i);

		if (d->class_code != 0x0C || d->subclass != 0x03)
			continue;
		n++;
		console_printf("%02x:%02x.%u  %04x:%04x  %s\n", d->bus, d->slot, d->func, d->vendor, d->device, kind_name(d->prog_if));
		if (d->prog_if == 0x00) { /* UHCI: I/O base in BAR4; USBCMD at +0, USBSTS at +2, two root ports at +0x10 */
			uint16_t io = (uint16_t)(d->bar[4] & ~3u);

			console_printf("        I/O ports %04x, command %04x, status %04x, irq %u\n", io, inw(io), inw(io + 2), d->irq_line);
		} else if (d->prog_if == 0x20 || d->prog_if == 0x30) {
			uint32_t base = d->bar[0] & ~0xFu;
			volatile uint8_t *cap;
			uint32_t hcs1, ports, ver;

			if (!base || map(base, 0x1000)) {
				console_write("        registers not mapped\n");
				continue;
			}
			cap = (volatile uint8_t *)base;
			ver = *(volatile uint32_t *)cap >> 16; /* HCIVERSION: the upper half of the first dword */
			hcs1 = *(volatile uint32_t *)(cap + 4);
			ports = d->prog_if == 0x20 ? (hcs1 & 0xF) : (hcs1 >> 24);
			console_printf("        registers at %08x, interface version %x.%02x, %u root port(s), irq %u\n", base,
				       ver >> 8, ver & 0xFF, ports, d->irq_line);
		} else {
			console_printf("        BAR0 %08x, irq %u\n", d->bar[0], d->irq_line);
		}
	}
	if (!n)
		console_write("no USB controllers (QEMU: -device qemu-xhci | usb-ehci | piix3-usb-uhci)\n");
	return n ? 0 : 1;
}
