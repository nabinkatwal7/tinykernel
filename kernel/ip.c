#include "ip.h"

#include "arp.h"
#include "klog.h"
#include "kmalloc.h"
#include "timer.h"
#include "kstring.h"

#define IP_ETHERTYPE 0x0800

struct ip_header {
	uint8_t ver_ihl;          /* version (4) in the high nibble, header length in 32-bit words in the low */
	uint8_t tos;
	uint16_t total_len;       /* big endian from here on */
	uint16_t id;
	uint16_t frag;            /* flags (bit 14 = don't fragment) and fragment offset */
	uint8_t ttl;
	uint8_t proto;
	uint16_t checksum;
	uint8_t src[4], dst[4];
} __attribute__((packed));

#define MAX_PROTOS 6
static struct { uint8_t proto; ip_handler_t fn; } protos[MAX_PROTOS];
static struct ip_stats stats;
static uint16_t next_id = 1;

static uint32_t get_ip(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_ip(uint8_t *p, uint32_t ip)
{
	p[0] = (uint8_t)(ip >> 24);
	p[1] = (uint8_t)(ip >> 16);
	p[2] = (uint8_t)(ip >> 8);
	p[3] = (uint8_t)ip;
}

/* One's complement sum of 16-bit big-endian words, folded; the result is stored as-is in the header. */
uint16_t ip_checksum(const void *data, uint32_t len)
{
	const uint8_t *p = data;
	uint32_t sum = 0;

	while (len > 1) {
		sum += (uint32_t)((p[0] << 8) | p[1]);
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (uint32_t)(p[0] << 8); /* odd trailing byte is padded with zero */
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (uint16_t)~sum;
}

uint32_t ip_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len)
{
	uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;

	return sum;
}

void ip_get_stats(struct ip_stats *out)
{
	*out = stats;
}

int ip_register_proto(uint8_t proto, ip_handler_t handler)
{
	int i;

	for (i = 0; i < MAX_PROTOS; i++) {
		if (!protos[i].fn) {
			protos[i].proto = proto;
			protos[i].fn = handler;
			return 0;
		}
	}
	return -1;
}

/*
 * Reassembly. A fragment carries an offset (in 8-byte units) and a "more fragments" flag; the datagram is identified
 * by source, destination, protocol and id. Fragments are copied into a buffer at their offset and a bitmap of 8-byte
 * units says what has arrived; once the last fragment (no MF flag) has fixed the total length and every unit is
 * present, the datagram goes to its protocol handler as if it had arrived whole.
 */
#define REASM_SLOTS 4
#define REASM_TIMEOUT_S 15

static struct reasm {
	int used;
	uint32_t src, dst, started;
	uint16_t id;
	uint8_t proto;
	uint32_t total;             /* known once the last fragment arrived, else 0 */
	uint8_t have[IP_REASM_MAX / 8 / 8]; /* one bit per 8-byte unit */
	uint8_t *data;
} reasm[REASM_SLOTS];

static struct reasm *reasm_slot(uint32_t src, uint32_t dst, uint16_t id, uint8_t proto)
{
	struct reasm *free_slot = 0;
	int i;

	for (i = 0; i < REASM_SLOTS; i++) {
		struct reasm *r = &reasm[i];

		if (r->used && (timer_ticks() - r->started) / timer_hz() >= REASM_TIMEOUT_S) { /* give up on stale datagrams */
			r->used = 0;
			stats.reasm_timeouts++;
		}
		if (r->used && r->src == src && r->dst == dst && r->id == id && r->proto == proto)
			return r;
		if (!r->used && !free_slot)
			free_slot = r;
	}
	if (free_slot) {
		if (!free_slot->data)
			free_slot->data = kmalloc(IP_REASM_MAX);
		if (!free_slot->data)
			return 0;
		memset(free_slot->have, 0, sizeof free_slot->have);
		free_slot->used = 1;
		free_slot->src = src;
		free_slot->dst = dst;
		free_slot->id = id;
		free_slot->proto = proto;
		free_slot->total = 0;
		free_slot->started = timer_ticks();
	}
	return free_slot;
}

static int reasm_complete(const struct reasm *r)
{
	uint32_t units, u;

	if (!r->total)
		return 0;
	units = (r->total + 7) / 8;
	for (u = 0; u < units; u++)
		if (!(r->have[u / 8] & (1u << (u % 8))))
			return 0;
	return 1;
}

static void deliver(uint8_t proto, uint32_t src, uint32_t dst, const uint8_t *payload, uint16_t len)
{
	int i;

	stats.rx_packets++;
	for (i = 0; i < MAX_PROTOS; i++) {
		if (protos[i].fn && protos[i].proto == proto) {
			protos[i].fn(src, dst, payload, len);
			return;
		}
	}
}

static void ip_input(const uint8_t *frame, uint16_t len)
{
	const struct ip_header *h = (const struct ip_header *)(frame + ETH_HLEN);
	uint16_t hlen, total;
	uint32_t src, dst;

	if (len < ETH_HLEN + sizeof *h)
		return;
	hlen = (uint16_t)((h->ver_ihl & 0x0F) * 4);
	total = ntohs(h->total_len);
	if ((h->ver_ihl >> 4) != 4 || hlen < 20 || total < hlen || ETH_HLEN + total > len
	    || ip_checksum(h, hlen) != 0) { /* a valid header sums (with its checksum) to 0xFFFF -> ~ = 0 */
		stats.rx_bad++;
		return;
	}
	src = get_ip(h->src);
	dst = get_ip(h->dst);
	if (dst != netif.ip && dst >> 24 != 127 && dst != 0xFFFFFFFFu && dst != (netif.ip | ~netif.netmask)) {
		stats.rx_not_for_us++;
		return;
	}
	/* A packet from a neighbour tells us its MAC address: remember it, so answering it (often from the receive task
	   itself, which could not wait for an ARP reply) does not need a lookup on the network. */
	if (netif.netmask && src != netif.ip && ((src ^ netif.ip) & netif.netmask) == 0)
		arp_learn(src, frame + ETH_ALEN);
	if (ntohs(h->frag) & 0x3FFF) { /* a fragment (MF flag or a non-zero offset) */
		uint16_t frag = ntohs(h->frag), plen = (uint16_t)(total - hlen);
		uint32_t off = (uint32_t)(frag & 0x1FFF) * 8, u;
		struct reasm *r;

		stats.frags_in++;
		if (((frag & 0x2000) && (plen & 7)) || off + plen > IP_REASM_MAX || !plen) { /* middle fragments are multiples of 8 */
			stats.reasm_dropped++;
			return;
		}
		r = reasm_slot(src, dst, ntohs(h->id), h->proto);
		if (!r) {
			stats.reasm_dropped++;
			return;
		}
		memcpy(r->data + off, (const uint8_t *)h + hlen, plen);
		for (u = off / 8; u < (off + plen + 7) / 8; u++)
			r->have[u / 8] |= (uint8_t)(1u << (u % 8));
		if (!(frag & 0x2000))
			r->total = off + plen; /* the last fragment fixes the length */
		if (reasm_complete(r)) {
			r->used = 0;
			stats.reassembled++;
			deliver(r->proto, src, dst, r->data, (uint16_t)r->total);
		}
		return;
	}
	deliver(h->proto, src, dst, (const uint8_t *)h + hlen, (uint16_t)(total - hlen));
}

/* Build and send one IP packet (a whole datagram or one fragment). */
static int send_packet(uint32_t dst, uint8_t proto, uint16_t id, uint16_t frag, const uint8_t *mac, const void *payload, uint16_t len)
{
	uint8_t pkt[ETH_MAX_FRAME - ETH_HLEN];
	struct ip_header *h = (struct ip_header *)pkt;

	h->ver_ihl = 0x45;
	h->tos = 0;
	h->total_len = htons((uint16_t)(sizeof *h + len));
	h->id = htons(id);
	h->frag = htons(frag);
	h->ttl = 64;
	h->proto = proto;
	h->checksum = 0;
	put_ip(h->src, dst >> 24 == 127 ? dst : netif.ip); /* loopback traffic comes from 127.x.x.x */
	put_ip(h->dst, dst);
	h->checksum = htons(ip_checksum(h, sizeof *h));
	memcpy(pkt + sizeof *h, payload, len);
	stats.tx_packets++;
	if (!mac) {
		/* to ourselves: no wire involved, hand the frame straight to the receive path */
		uint8_t loop[ETH_MAX_FRAME];

		memset(loop, 0, ETH_HLEN);
		loop[12] = 0x08;
		memcpy(loop + ETH_HLEN, pkt, sizeof *h + len);
		ip_input(loop, (uint16_t)(ETH_HLEN + sizeof *h + len));
		return 0;
	}
	return eth_send(mac, IP_ETHERTYPE, pkt, (uint16_t)(sizeof *h + len));
}

#define MAX_PAYLOAD (ETH_MAX_FRAME - ETH_HLEN - 20) /* 1480: what one packet carries; fragments use multiples of 8 */

int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len)
{
	uint8_t macbuf[ETH_ALEN];
	const uint8_t *mac = macbuf;
	uint32_t next_hop = dst, off = 0;
	uint16_t id = next_id++;
	const uint8_t *p = payload;

	if (!netif.up || len > IP_REASM_MAX)
		return -1;
	if ((dst & netif.netmask) != (netif.ip & netif.netmask) && dst != 0xFFFFFFFFu)
		next_hop = netif.gateway; /* off link: hand it to the router */
	if (dst == netif.ip || dst >> 24 == 127) {
		mac = 0;
	} else if (dst == 0xFFFFFFFFu) {
		memset(macbuf, 0xFF, ETH_ALEN);
	} else if (arp_resolve(next_hop, macbuf, 2000)) {
		stats.tx_arp_fail++;
		return -1;
	}
	if (len <= MAX_PAYLOAD)
		return send_packet(dst, proto, id, 0x4000 /* don't fragment */, mac, payload, len);
	while (off < len) { /* too big for one packet: send fragments, every one but the last carrying 1480 bytes */
		uint16_t chunk = len - off > MAX_PAYLOAD ? MAX_PAYLOAD : (uint16_t)(len - off);
		uint16_t frag = (uint16_t)(off / 8) | (off + chunk < len ? 0x2000 : 0);

		stats.frags_out++;
		if (send_packet(dst, proto, id, frag, mac, p + off, chunk))
			return -1;
		off += chunk;
	}
	return 0;
}

void ip_init(void)
{
	net_register_ethertype(IP_ETHERTYPE, ip_input);
}
