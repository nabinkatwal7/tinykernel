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

#define REG_TSD(n)  (REG_TSD0 + 4 * (n))
#define REG_TSAD(n) (REG_TSAD0 + 4 * (n))
#define TSD_OWN 0x2000
#define TSD_TUN 0x4000
#define TSD_TOK 0x8000
#define RX_BUF_LEN (8192 + 16)
#define TX_SLOT    2048
#define NTX        4

struct netif netif;

static uint8_t *rx_buf;          /* DMA ring the card writes received frames into */
static uint8_t *tx_buf[NTX];     /* one buffer per transmit descriptor */
static int tx_cur;

const char *mac_str(const uint8_t mac[ETH_ALEN], char out[18])
{
	ksnprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return out;
}

/* Send one frame: pad it to the 60-byte minimum, copy into a DMA buffer, hand it to the card. */
int net_send_frame(const void *frame, uint16_t len)
{
	uint16_t io = netif.io_base;
	int slot = tx_cur, spin;
	uint32_t tsd;

	if (!netif.up || len < ETH_HLEN || len > ETH_MAX_FRAME)
		return -1;
	/* the previous user of this descriptor must have finished (OWN set means the DMA is done) */
	for (spin = 100000; spin; spin--) {
		tsd = inl((uint16_t)(io + REG_TSD(slot)));
		if (tsd & (TSD_OWN | TSD_TOK) || !tsd)
			break;
	}
	memcpy(tx_buf[slot], frame, len);
	if (len < ETH_MIN_FRAME) {
		memset(tx_buf[slot] + len, 0, (size_t)(ETH_MIN_FRAME - len));
		len = ETH_MIN_FRAME;
	}
	outl((uint16_t)(io + REG_TSAD(slot)), (uint32_t)tx_buf[slot]);
	outl((uint16_t)(io + REG_TSD(slot)), len); /* writing the length (OWN clear) starts the transfer */
	for (spin = 1000000; spin; spin--) {
		tsd = inl((uint16_t)(io + REG_TSD(slot)));
		if (tsd & (TSD_TOK | TSD_TUN))
			break;
	}
	tx_cur = (tx_cur + 1) % NTX;
	if (!(tsd & TSD_TOK)) {
		netif.tx_errors++;
		return -1;
	}
	netif.tx_frames++;
	return 0;
}

int eth_send(const uint8_t dst[ETH_ALEN], uint16_t ethertype, const void *payload, uint16_t len)
{
	uint8_t frame[ETH_MAX_FRAME];

	if (len > ETH_MAX_FRAME - ETH_HLEN)
		return -1;
	memcpy(frame, dst, ETH_ALEN);
	memcpy(frame + ETH_ALEN, netif.mac, ETH_ALEN);
	frame[12] = (uint8_t)(ethertype >> 8);
	frame[13] = (uint8_t)ethertype;
	memcpy(frame + ETH_HLEN, payload, len);
	return net_send_frame(frame, (uint16_t)(ETH_HLEN + len));
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
	/* DMA memory must be physically contiguous: the allocator hands out identity-mapped frames */
	{
		uint32_t rx = pmm_alloc_contig(3), tx = pmm_alloc_contig(2);

		if (!rx || !tx) {
			klog(LOG_WARN, "net: out of DMA memory");
			return -1;
		}
		rx_buf = (uint8_t *)rx;
		memset(rx_buf, 0, RX_BUF_LEN);
		for (i = 0; i < NTX; i++)
			tx_buf[i] = (uint8_t *)(tx + (uint32_t)i * TX_SLOT);
	}
	outl((uint16_t)(io + REG_RBSTART), (uint32_t)rx_buf);
	outw((uint16_t)(io + REG_IMR), 0x0000);        /* polling until the IRQ path is added */
	outl((uint16_t)(io + REG_RCR), 0x0000008F);    /* accept broadcast/multicast/physical/all, WRAP */
	outl((uint16_t)(io + REG_TCR), 0x03000700);    /* default inter-frame gap and DMA burst */
	outb((uint16_t)(io + REG_CR), CR_RE | CR_TE);
	netif.up = 1;
	{
		char m[18];

		klog(LOG_INFO, "net: RTL8139 at io %x irq %u, MAC %s, link %s", io, netif.irq,
		     mac_str(netif.mac, m), rtl8139_link_up() ? "up" : "down");
	}
	return 0;
}
