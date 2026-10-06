#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>

void icmp_init(void);

/*
 * Send one echo request to 'dst' and wait for the reply. Returns the round trip time in
 * milliseconds (0 means "under a tick"), or a negative value: -1 could not send (no route/ARP),
 * -2 timed out. 'bytes' receives the size of the reply's ICMP payload.
 */
int icmp_ping(uint32_t dst, uint16_t seq, uint32_t timeout_ms, uint32_t *bytes, uint8_t *ttl);

struct icmp_stats {
	uint32_t echo_requests_in, echo_replies_out, echo_replies_in, echo_requests_out;
};
void icmp_get_stats(struct icmp_stats *out);

#endif
