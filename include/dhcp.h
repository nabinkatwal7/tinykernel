#ifndef DHCP_H
#define DHCP_H

#include <stdint.h>

/*
 * Minimal DHCP client (RFC 2131): DISCOVER -> OFFER -> REQUEST -> ACK over UDP 68/67 with the
 * broadcast flag set. On success it fills netif.ip/netmask/gateway/dns. Returns 0, or a negative
 * value: -1 no usable socket / no interface, -2 no OFFER, -3 no ACK (or a NAK).
 */
int dhcp_acquire(uint32_t timeout_ms);
uint32_t dhcp_lease_seconds(void);   /* lease length from the last ACK, 0 if none */

#endif
