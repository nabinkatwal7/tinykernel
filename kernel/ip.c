#include "ip.h"

#include "arp.h"
#include "klog.h"
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

#define MAX_PROTOS 4
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

static void ip_input(const uint8_t *frame, uint16_t len)
{
	const struct ip_header *h = (const struct ip_header *)(frame + ETH_HLEN);
	uint16_t hlen, total;
	uint32_t src, dst;
	int i;

	if (len < ETH_HLEN + sizeof *h)
		return;
	hlen = (uint16_t)((h->ver_ihl & 0x0F) * 4);
	total = ntohs(h->total_len);
	if ((h->ver_ihl >> 4) != 4 || hlen < 20 || total < hlen || ETH_HLEN + total > len
	    || ip_checksum(h, hlen) != 0) { /* a valid header sums (with its checksum) to 0xFFFF -> ~ = 0 */
		stats.rx_bad++;
		return;
	}
	if (ntohs(h->frag) & 0x3FFF) { /* fragments (MF flag or offset): not supported */
		stats.rx_bad++;
		return;
	}
	src = get_ip(h->src);
	dst = get_ip(h->dst);
	if (dst != netif.ip && dst >> 24 != 127 && dst != 0xFFFFFFFFu && dst != (netif.ip | ~netif.netmask)) {
		stats.rx_not_for_us++;
		return;
	}
	stats.rx_packets++;
	for (i = 0; i < MAX_PROTOS; i++) {
		if (protos[i].fn && protos[i].proto == h->proto) {
			protos[i].fn(src, dst, (const uint8_t *)h + hlen, (uint16_t)(total - hlen));
			return;
		}
	}
}

int ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len)
{
	uint8_t pkt[ETH_MAX_FRAME - ETH_HLEN];
	struct ip_header *h = (struct ip_header *)pkt;
	uint8_t mac[ETH_ALEN];
	uint32_t next_hop = dst;

	if (!netif.up || len > sizeof pkt - sizeof *h)
		return -1;
	if ((dst & netif.netmask) != (netif.ip & netif.netmask) && dst != 0xFFFFFFFFu)
		next_hop = netif.gateway; /* off link: hand it to the router */

	h->ver_ihl = 0x45;
	h->tos = 0;
	h->total_len = htons((uint16_t)(sizeof *h + len));
	h->id = htons(next_id++);
	h->frag = htons(0x4000); /* don't fragment */
	h->ttl = 64;
	h->proto = proto;
	h->checksum = 0;
	put_ip(h->src, dst >> 24 == 127 ? dst : netif.ip); /* loopback traffic comes from 127.x.x.x */
	put_ip(h->dst, dst);
	h->checksum = htons(ip_checksum(h, sizeof *h));
	memcpy(pkt + sizeof *h, payload, len);

	if (dst == netif.ip || dst >> 24 == 127) {
		/* to ourselves: no wire involved, hand the frame straight to the receive path */
		uint8_t loop[ETH_MAX_FRAME];

		memset(loop, 0, ETH_HLEN);
		loop[12] = 0x08;
		memcpy(loop + ETH_HLEN, pkt, sizeof *h + len);
		stats.tx_packets++;
		ip_input(loop, (uint16_t)(ETH_HLEN + sizeof *h + len));
		return 0;
	}
	if (dst == 0xFFFFFFFFu) {
		memset(mac, 0xFF, ETH_ALEN);
	} else if (arp_resolve(next_hop, mac, 2000)) {
		stats.tx_arp_fail++;
		return -1;
	}
	stats.tx_packets++;
	return eth_send(mac, IP_ETHERTYPE, pkt, (uint16_t)(sizeof *h + len));
}

void ip_init(void)
{
	net_register_ethertype(IP_ETHERTYPE, ip_input);
}
