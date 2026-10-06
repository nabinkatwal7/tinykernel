#include "net.h"

#include "console.h"
#include "idt.h"
#include "io.h"
#include "pic.h"
#include "sched.h"
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

#define RXQ 16
#define RX_FRAME_MAX 1536
static struct { uint16_t len; uint8_t data[RX_FRAME_MAX]; } rxq[RXQ];
static volatile int rxq_head, rxq_tail;
static struct waitq rx_wq;
static uint32_t rx_off;       /* where the driver has read up to in the card's ring */
static int trace;

#define MAX_HANDLERS 8
static struct { uint16_t type; eth_handler_t fn; } handlers[MAX_HANDLERS];

int net_register_ethertype(uint16_t type, eth_handler_t fn)
{
	int i;

	for (i = 0; i < MAX_HANDLERS; i++) {
		if (!handlers[i].fn) {
			handlers[i].type = type;
			handlers[i].fn = fn;
			return 0;
		}
	}
	return -1;
}

void net_set_trace(int on)
{
	trace = on;
}

void net_set_loopback(int on)
{
	uint32_t tcr = inl((uint16_t)(netif.io_base + REG_TCR));

	tcr = on ? (tcr | 0x60000u) : (tcr & ~0x60000u); /* LBK1:LBK0 = 11 */
	outl((uint16_t)(netif.io_base + REG_TCR), tcr);
}

/* IRQ context: move every complete frame out of the card's ring into our queue. */
static void rx_drain(void)
{
	uint16_t io = netif.io_base;

	while (!(inb((uint16_t)(io + REG_CR)) & CR_BUFE)) {
		uint8_t *p = rx_buf + rx_off;
		uint16_t status = (uint16_t)(p[0] | (p[1] << 8)), len = (uint16_t)(p[2] | (p[3] << 8));
		int next = (rxq_head + 1) % RXQ;

		if (!(status & 1) || len < 18 || len > ETH_MAX_FRAME + 4) { /* bad packet: resync the ring */
			netif.rx_errors++;
			outb((uint16_t)(io + REG_CR), CR_TE);          /* stop the receiver ... */
			outb((uint16_t)(io + REG_CR), CR_RE | CR_TE);  /* ... and restart it */
			rx_off = 0;
			outw((uint16_t)(io + REG_CAPR), (uint16_t)(rx_off - 16));
			outl((uint16_t)(io + REG_RBSTART), (uint32_t)rx_buf);
			return;
		}
		if (next == rxq_tail) {
			netif.rx_dropped++;
		} else {
			uint16_t flen = (uint16_t)(len - 4);                /* drop the CRC */

			memcpy(rxq[rxq_head].data, p + 4, flen);
			rxq[rxq_head].len = flen;
			rxq_head = next;
		}
		netif.rx_frames++;
		rx_off = (rx_off + len + 4 + 3u) & ~3u;
		if (rx_off >= 8192)
			rx_off -= 8192;
		outw((uint16_t)(io + REG_CAPR), (uint16_t)(rx_off - 16));
	}
}

static void rtl_irq(struct regs *r)
{
	uint16_t io = netif.io_base, isr = inw((uint16_t)(io + REG_ISR));

	(void)r;
	outw((uint16_t)(io + REG_ISR), isr); /* acknowledge everything we saw */
	if (isr & 0x01)
		rx_drain();
	if (isr & 0x10) /* rx buffer overflow: drain what is there */
		rx_drain();
	if (rxq_head != rxq_tail)
		wq_wake_one(&rx_wq);
}

static void dispatch(const uint8_t *f, uint16_t len)
{
	uint16_t type = (uint16_t)((f[12] << 8) | f[13]);
	int i;

	if (trace) {
		char s[18], d[18];

		console_printf("[net] rx %u bytes %s -> %s type %04x\n", len, mac_str(f + 6, s), mac_str(f, d), type);
	}
	for (i = 0; i < MAX_HANDLERS; i++) {
		if (handlers[i].fn && handlers[i].type == type) {
			handlers[i].fn(f, len);
			return;
		}
	}
}

static void rx_task(void *arg)
{
	static uint8_t local[RX_FRAME_MAX];

	(void)arg;
	for (;;) {
		uint32_t flags = irq_save();
		uint16_t len;

		while (rxq_head == rxq_tail)
			wq_wait(&rx_wq, WAIT_OTHER);
		len = rxq[rxq_tail].len;
		memcpy(local, rxq[rxq_tail].data, len);
		rxq_tail = (rxq_tail + 1) % RXQ;
		irq_restore(flags);
		dispatch(local, len);
	}
}

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
	outw((uint16_t)(io + REG_IMR), 0x001F);        /* ROK, RER, TOK, TER, receive overflow */
	outl((uint16_t)(io + REG_RCR), 0x0000008F);    /* accept broadcast/multicast/physical/all, WRAP */
	outl((uint16_t)(io + REG_TCR), 0x03000700);    /* default inter-frame gap and DMA burst */
	outb((uint16_t)(io + REG_CR), CR_RE | CR_TE);
	netif.up = 1;
	irq_install_handler(netif.irq, rtl_irq);
	pic_unmask(netif.irq);
	task_create("netrx", rx_task, 0, 6);
	{
		char m[18];

		klog(LOG_INFO, "net: RTL8139 at io %x irq %u, MAC %s, link %s", io, netif.irq,
		     mac_str(netif.mac, m), rtl8139_link_up() ? "up" : "down");
	}
	return 0;
}
