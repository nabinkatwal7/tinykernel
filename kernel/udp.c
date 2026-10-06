#include "udp.h"

#include "io.h"
#include "ip.h"
#include "kmalloc.h"
#include "kstring.h"
#include "sched.h"
#include "timer.h"

struct udp_header {
	uint16_t src_port, dst_port, length, checksum; /* big endian */
} __attribute__((packed));

struct datagram {
	uint32_t src_ip;
	uint16_t src_port, len;
	uint8_t data[UDP_MAX_PAYLOAD];
};

struct socket {
	int used;
	uint16_t port;
	struct datagram q[UDP_QUEUE];
	int head, tail;
	struct waitq wq;
};

static struct socket *socks[UDP_MAX_SOCKETS]; /* allocated on open: the receive queues are big */
static struct udp_stats stats;
static uint16_t next_ephemeral = 49152;

void udp_get_stats(struct udp_stats *out)
{
	*out = stats;
}

static int port_in_use(uint16_t port)
{
	int i;

	for (i = 0; i < UDP_MAX_SOCKETS; i++)
		if (socks[i] && socks[i]->port == port)
			return 1;
	return 0;
}

int udp_open(uint16_t port)
{
	uint32_t flags = irq_save();
	int i;

	if (port == 0) {
		do {
			port = next_ephemeral++;
			if (next_ephemeral == 0)
				next_ephemeral = 49152;
		} while (port_in_use(port));
	} else if (port_in_use(port)) {
		irq_restore(flags);
		return -1;
	}
	for (i = 0; i < UDP_MAX_SOCKETS; i++) {
		if (!socks[i]) {
			socks[i] = kcalloc(1, sizeof *socks[i]);
			if (!socks[i])
				break;
			socks[i]->used = 1;
			socks[i]->port = port;
			irq_restore(flags);
			return i;
		}
	}
	irq_restore(flags);
	return -1;
}

void udp_close(int sock)
{
	if (sock >= 0 && sock < UDP_MAX_SOCKETS && socks[sock]) {
		uint32_t flags = irq_save();
		struct socket *s = socks[sock];

		socks[sock] = 0;      /* unpublish first: the receive path must not find it any more */
		irq_restore(flags);
		kfree(s);
	}
}

uint16_t udp_local_port(int sock)
{
	return sock >= 0 && sock < UDP_MAX_SOCKETS && socks[sock] ? socks[sock]->port : 0;
}

/* The checksum covers a pseudo header (addresses, protocol, length) plus the whole UDP datagram. */
static uint16_t udp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len)
{
	uint32_t sum = ip_pseudo_sum(src, dst, IP_PROTO_UDP, len), i;

	for (i = 0; i + 1 < len; i += 2)
		sum += (uint32_t)((seg[i] << 8) | seg[i + 1]);
	if (len & 1)
		sum += (uint32_t)(seg[len - 1] << 8);
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)~sum;
}

int udp_sendto(int sock, uint32_t dst_ip, uint16_t dst_port, const void *data, uint16_t len)
{
	uint8_t pkt[sizeof(struct udp_header) + UDP_MAX_PAYLOAD];
	struct udp_header *h = (struct udp_header *)pkt;
	uint32_t src_ip = dst_ip >> 24 == 127 ? dst_ip : netif.ip;
	uint16_t total = (uint16_t)(sizeof *h + len), sum;

	if (sock < 0 || sock >= UDP_MAX_SOCKETS || !socks[sock] || len > UDP_MAX_PAYLOAD)
		return -1;
	h->src_port = htons(socks[sock]->port);
	h->dst_port = htons(dst_port);
	h->length = htons(total);
	h->checksum = 0;
	memcpy(pkt + sizeof *h, data, len);
	sum = udp_checksum(src_ip, dst_ip, pkt, total);
	h->checksum = htons(sum ? sum : 0xFFFF); /* 0 would mean "no checksum" */
	stats.tx++;
	return ip_send(dst_ip, IP_PROTO_UDP, pkt, total);
}

static void udp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len)
{
	const struct udp_header *h = (const struct udp_header *)seg;
	uint16_t total, dport, plen;
	int i;
	struct socket *s = 0;

	if (len < sizeof *h)
		return;
	total = ntohs(h->length);
	if (total < sizeof *h || total > len)
		return;
	if (h->checksum && udp_checksum(src, dst, seg, total) != 0) {
		stats.rx_bad_checksum++;
		return;
	}
	dport = ntohs(h->dst_port);
	plen = (uint16_t)(total - sizeof *h);
	for (i = 0; i < UDP_MAX_SOCKETS; i++)
		if (socks[i] && socks[i]->port == dport)
			s = socks[i];
	if (!s) {
		stats.rx_no_socket++;
		return;
	}
	{
		uint32_t flags = irq_save();
		int next = (s->head + 1) % UDP_QUEUE;

		if (next == s->tail) {
			stats.rx_dropped++;
		} else {
			s->q[s->head].src_ip = src;
			s->q[s->head].src_port = ntohs(h->src_port);
			s->q[s->head].len = plen;
			memcpy(s->q[s->head].data, seg + sizeof *h, plen);
			s->head = next;
			stats.rx++;
			wq_wake_one(&s->wq);
		}
		irq_restore(flags);
	}
}

int udp_recvfrom(int sock, void *buf, uint16_t cap, uint32_t *src_ip, uint16_t *src_port, uint32_t timeout_ms)
{
	uint32_t deadline = timer_ticks() + timeout_ms * timer_hz() / 1000;
	struct socket *s;

	if (sock < 0 || sock >= UDP_MAX_SOCKETS || !socks[sock])
		return -1;
	s = socks[sock];
	for (;;) {
		uint32_t flags = irq_save();

		if (s->head != s->tail) {
			struct datagram *d = &s->q[s->tail];
			uint16_t n = d->len < cap ? d->len : cap;

			memcpy(buf, d->data, n);
			if (src_ip)
				*src_ip = d->src_ip;
			if (src_port)
				*src_port = d->src_port;
			s->tail = (s->tail + 1) % UDP_QUEUE;
			irq_restore(flags);
			return n;
		}
		irq_restore(flags);
		if ((int32_t)(deadline - timer_ticks()) <= 0)
			return -1;
		task_sleep(10);
	}
}

void udp_init(void)
{
	ip_register_proto(IP_PROTO_UDP, udp_input);
}
