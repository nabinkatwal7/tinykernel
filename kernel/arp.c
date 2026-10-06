#include "arp.h"

#include "io.h"
#include "kstring.h"
#include "sched.h"
#include "timer.h"

#define ARP_ETHERTYPE 0x0806
#define OP_REQUEST    1
#define OP_REPLY      2

struct arp_packet {
	uint8_t htype[2], ptype[2], hlen, plen, oper[2];
	uint8_t sha[6], spa[4], tha[6], tpa[4];
} __attribute__((packed));

static struct {
	int used;
	uint32_t ip;
	uint8_t mac[ETH_ALEN];
	uint32_t stamp;   /* when it was learned, in ticks */
} cache[ARP_CACHE_SIZE];

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

static void learn(uint32_t ip, const uint8_t mac[ETH_ALEN])
{
	int i, victim = 0;
	uint32_t flags = irq_save();

	for (i = 0; i < ARP_CACHE_SIZE; i++) {
		if (cache[i].used && cache[i].ip == ip) {
			victim = i;
			goto fill;
		}
	}
	for (i = 0; i < ARP_CACHE_SIZE; i++) {
		if (!cache[i].used) {
			victim = i;
			goto fill;
		}
		if (cache[i].stamp < cache[victim].stamp)
			victim = i; /* evict the oldest */
	}
fill:
	cache[victim].used = 1;
	cache[victim].ip = ip;
	memcpy(cache[victim].mac, mac, ETH_ALEN);
	cache[victim].stamp = timer_ticks();
	irq_restore(flags);
}

void arp_learn(uint32_t ip, const uint8_t mac[ETH_ALEN])
{
	learn(ip, mac);
}

/*
 * Aging: an entry is only trusted for arp_ttl seconds after the last time we heard from that neighbour (every ARP
 * packet and every IP packet from it refreshes the stamp). A stale entry is dropped and the next lookup asks again,
 * so a host whose MAC address changed is found again.
 */
static uint32_t arp_ttl = 60;
static uint32_t arp_expired;

static int stale(int i)
{
	return (timer_ticks() - cache[i].stamp) / timer_hz() >= arp_ttl;
}

void arp_set_ttl(uint32_t seconds)
{
	arp_ttl = seconds ? seconds : 1;
}

uint32_t arp_get_ttl(void)
{
	return arp_ttl;
}

uint32_t arp_expired_count(void)
{
	return arp_expired;
}

int arp_cache_age(int index)
{
	if (index < 0 || index >= ARP_CACHE_SIZE || !cache[index].used)
		return -1;
	return (int)((timer_ticks() - cache[index].stamp) / timer_hz());
}

int arp_lookup(uint32_t ip, uint8_t mac[ETH_ALEN])
{
	int i;
	uint32_t flags = irq_save();

	for (i = 0; i < ARP_CACHE_SIZE; i++) {
		if (cache[i].used && cache[i].ip == ip) {
			if (stale(i)) { /* too old to trust: forget it */
				cache[i].used = 0;
				arp_expired++;
				break;
			}
			memcpy(mac, cache[i].mac, ETH_ALEN);
			irq_restore(flags);
			return 0;
		}
	}
	irq_restore(flags);
	return -1;
}

int arp_cache_get(int index, uint32_t *ip, uint8_t mac[ETH_ALEN])
{
	if (index < 0 || index >= ARP_CACHE_SIZE || !cache[index].used)
		return -1;
	if (stale(index)) {
		cache[index].used = 0;
		arp_expired++;
		return -1;
	}
	*ip = cache[index].ip;
	memcpy(mac, cache[index].mac, ETH_ALEN);
	return 0;
}

void arp_cache_clear(void)
{
	uint32_t flags = irq_save();

	memset(cache, 0, sizeof cache);
	irq_restore(flags);
}

static void send_arp(uint16_t oper, const uint8_t dst_mac[ETH_ALEN], const uint8_t tha[ETH_ALEN], uint32_t tpa)
{
	struct arp_packet p;

	p.htype[0] = 0; p.htype[1] = 1;
	p.ptype[0] = 0x08; p.ptype[1] = 0x00;
	p.hlen = ETH_ALEN;
	p.plen = 4;
	p.oper[0] = (uint8_t)(oper >> 8);
	p.oper[1] = (uint8_t)oper;
	memcpy(p.sha, netif.mac, ETH_ALEN);
	put_ip(p.spa, netif.ip);
	memcpy(p.tha, tha, ETH_ALEN);
	put_ip(p.tpa, tpa);
	eth_send(dst_mac, ARP_ETHERTYPE, &p, sizeof p);
}

static void arp_input(const uint8_t *frame, uint16_t len)
{
	const struct arp_packet *p = (const struct arp_packet *)(frame + ETH_HLEN);
	uint16_t oper;
	uint32_t spa, tpa;

	if (len < ETH_HLEN + sizeof *p || p->htype[1] != 1 || p->ptype[0] != 0x08 || p->ptype[1] != 0x00
	    || p->hlen != ETH_ALEN || p->plen != 4)
		return;
	oper = (uint16_t)((p->oper[0] << 8) | p->oper[1]);
	spa = get_ip(p->spa);
	tpa = get_ip(p->tpa);
	if (spa)
		learn(spa, p->sha); /* both requests and replies tell us who owns spa */
	if (oper == OP_REQUEST && tpa == netif.ip && netif.ip)
		send_arp(OP_REPLY, p->sha, p->sha, spa);
}

int arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN], uint32_t timeout_ms)
{
	static const uint8_t bcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	static const uint8_t zero[ETH_ALEN];
	uint32_t waited = 0;

	if (!netif.up)
		return -1;
	if (!arp_lookup(ip, mac))
		return 0;
	while (waited < timeout_ms) {
		if (waited % 1000 == 0) /* (re)send the request every second */
			send_arp(OP_REQUEST, bcast, zero, ip);
		task_sleep(10);
		waited += 10;
		if (!arp_lookup(ip, mac))
			return 0;
	}
	return -1;
}

void arp_init(void)
{
	net_register_ethertype(ARP_ETHERTYPE, arp_input);
}
