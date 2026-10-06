#include "ahci.h"

#include "console.h"
#include "kstring.h"
#include "paging.h"
#include "pci.h"

/*
 * AHCI (SATA) basics: find the controller on the PCI bus, map its registers (ABAR, BAR5) and look at what is
 * plugged in. Reading and writing sectors would need command lists and FIS structures; this driver stops at
 * discovery. The register layout is described in docs/ahci.md.
 */
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS  0x08
#define HBA_PI  0x0C
#define HBA_VS  0x10
#define PORT_BASE(n) (0x100 + (n) * 0x80)
#define PORT_CMD  0x18
#define PORT_TFD  0x20
#define PORT_SIG  0x24
#define PORT_SSTS 0x28

static const struct pci_dev *find_controller(void)
{
	int i;

	for (i = 0; i < pci_count(); i++) {
		const struct pci_dev *d = pci_get(i);

		if (d->class_code == 0x01 && d->subclass == 0x06 && d->prog_if == 0x01)
			return d;
	}
	return 0;
}

static int popcount(uint32_t v)
{
	int n = 0;

	for (; v; v &= v - 1)
		n++;
	return n;
}

static volatile uint32_t *regs;

static uint32_t rd(uint32_t off)
{
	return regs[off / 4];
}

static int map_registers(const struct pci_dev *d)
{
	uint32_t base = d->bar[5] & ~0xFu, off, cmd;

	if (!base)
		return -1;
	cmd = pci_read32(d->bus, d->slot, d->func, 0x04);
	pci_write32(d->bus, d->slot, d->func, 0x04, cmd | 0x06); /* memory space + bus master */
	for (off = 0; off < 0x3000; off += 4096)
		if (paging_map(0, base + off, base + off, PTE_RW | PTE_PCD))
			return -1;
	regs = (volatile uint32_t *)base;
	return 0;
}

int cmd_ahci(int argc, char **argv)
{
	const struct pci_dev *d = find_controller();
	uint32_t cap, pi, vs, n, ports = 0;

	(void)argc;
	(void)argv;
	if (!d) {
		console_write("ahci: no AHCI controller found (QEMU: -device ahci,id=ahci ... -device ide-hd,bus=ahci.0)\n");
		return 1;
	}
	if (map_registers(d)) {
		console_write("ahci: cannot map the controller registers\n");
		return 1;
	}
	cap = rd(HBA_CAP);
	pi = rd(HBA_PI);
	vs = rd(HBA_VS);
	console_printf("AHCI controller %04x:%04x at %02x:%02x.%u, registers at %08x\n", d->vendor, d->device, d->bus, d->slot,
		       d->func, d->bar[5] & ~0xFu);
	console_printf("  version %u.%u%u, %u port(s) implemented of %u, %u command slots, %s addressing, global control %08x\n",
		       vs >> 16, (vs >> 8) & 0xFF, vs & 0xFF, popcount(pi), (cap & 31) + 1, ((cap >> 8) & 31) + 1,
		       (cap & 0x80000000u) ? "64-bit" : "32-bit", rd(HBA_GHC));
	for (n = 0; n < 32; n++) {
		uint32_t base, ssts, sig;
		const char *what;

		if (!(pi & (1u << n)))
			continue;
		base = PORT_BASE(n);
		ssts = rd(base + PORT_SSTS);
		sig = rd(base + PORT_SIG);
		if ((ssts & 0xF) != 3) { /* DET: 3 = device present and communication established */
			console_printf("  port %u: nothing attached (SSTS %08x)\n", n, ssts);
			continue;
		}
		switch (sig) {
		case 0x00000101: what = "SATA disk"; break;
		case 0xEB140101: what = "ATAPI (optical) device"; break;
		case 0xC33C0101: what = "enclosure management bridge"; break;
		case 0x96690101: what = "port multiplier"; break;
		default: what = "unknown device"; break;
		}
		ports++;
		console_printf("  port %u: %s, link speed gen%u, signature %08x, command engine %s\n", n, what, (ssts >> 4) & 0xF, sig,
			       (rd(base + PORT_CMD) & 1) ? "running" : "stopped");
	}
	console_printf("  %u device(s) attached\n", ports);
	return 0;
}
