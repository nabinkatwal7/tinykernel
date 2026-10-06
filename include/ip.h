#ifndef IP_H
#define IP_H

#include <stdint.h>

#include "net.h"

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* Receives the payload of an IPv4 packet (header stripped). Addresses are host byte order. */
typedef void (*ip_handler_t)(uint32_t src, uint32_t dst, const uint8_t *payload, uint16_t len);

void     ip_init(void);                                   /* hooks IPv4 into the Ethernet layer */
int      ip_register_proto(uint8_t proto, ip_handler_t handler);
/* Wrap a payload in an IPv4 header and send it, ARP-resolving the next hop (the gateway if dst is off
   link). 0 on success, -1 if the next hop could not be resolved or the frame could not be sent. */
int      ip_send(uint32_t dst, uint8_t proto, const void *payload, uint16_t len);
/* Datagrams larger than the 1500-byte MTU are split into fragments on the way out and put back together on the way in
   (up to IP_REASM_MAX bytes, four at a time, discarded after 15 seconds). */
#define IP_REASM_MAX 8192

uint16_t ip_checksum(const void *data, uint32_t len);     /* Internet checksum (RFC 1071), network order ready */
/* Pseudo-header checksum seed for TCP/UDP. */
uint32_t ip_pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);

struct ip_stats {
	uint32_t rx_packets, rx_bad, rx_not_for_us, tx_packets, tx_arp_fail;
	uint32_t frags_in, reassembled, reasm_timeouts, reasm_dropped, frags_out;
};
void     ip_get_stats(struct ip_stats *out);

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v)
{
	return (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

#endif
