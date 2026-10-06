#include "net.h"

#include "io.h"
#include "kmalloc.h"
#include "klog.h"
#include "kprintf.h"
#include "kstring.h"
#include "pci.h"
#include "pmm.h"
#include "timer.h"

#define VENDOR_REALTEK 0x10EC
#define DEVICE_8139    0x8139

#define REG_IDR0     0x00
#define REG_TSD0     0x10
#define REG_TSAD0    0x20
#define REG_RBSTART  0x30
#define REG_CR       0x37
#define REG_CAPR     0x38
#define REG_CBR      0x3A
#define REG_IMR      0x3C
#define REG_ISR      0x3E
#define REG_TCR      0x40
#define REG_RCR      0x44
#define REG_MSR      0x58
#define REG_CONFIG1  0x52

#define CR_RESET  0x10
#define CR_RE     0x08
#define CR_TE     0x04
#define CR_BUFE   0x01

struct netif netif;

const char *mac_str(const uint8_t mac[ETH_ALEN], char out[18])
{
	ksnprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return out;
}

int rtl8139_link_up(void)
{
	return netif.up && !(inb((uint16_t)(netif.io_base + REG_MSR)) & 0x04); /* bit 2 = LINKB (inverted) */
}

int rtl8139_init(void)
{
	const struct pci_dev *d = pci_find(VENDOR_REALTEK, DEVICE_8139);
	uint16_t io;
	uint32_t cmd;
	int i, spin;

	memset(&netif, 0, sizeof netif);
	if (!d) {
		klog(LOG_INFO, "net: no RTL8139 found");
		return -1;
	}
	if (!(d->bar[0] & 1)) {
		klog(LOG_WARN, "net: RTL8139 BAR0 is not an I/O range");
		return -1;
	}
	io = (uint16_t)(d->bar[0] & ~3u);
	netif.io_base = io;
	netif.irq = d->irq_line;

	/* the card DMAs into our memory, so it must be a bus master */
	cmd = pci_read32(d->bus, d->slot, d->func, 0x04);
	pci_write32(d->bus, d->slot, d->func, 0x04, (cmd & 0xFFFF) | 0x07);

	outb((uint16_t)(io + REG_CONFIG1), 0x00);          /* power on */
	outb((uint16_t)(io + REG_CR), CR_RESET);           /* software reset */
	for (spin = 1000000; spin && (inb((uint16_t)(io + REG_CR)) & CR_RESET); spin--)
		;
	if (!spin) {
		klog(LOG_WARN, "net: RTL8139 reset timed out");
		return -1;
	}
	for (i = 0; i < ETH_ALEN; i++)
		netif.mac[i] = inb((uint16_t)(io + REG_IDR0 + i));
	netif.up = 1;
	{
		char m[18];

		klog(LOG_INFO, "net: RTL8139 at io %x irq %u, MAC %s, link %s", io, netif.irq,
		     mac_str(netif.mac, m), rtl8139_link_up() ? "up" : "down");
	}
	return 0;
}
