#ifndef ARP_H
#define ARP_H

#include <stdint.h>

#include "net.h"

#define ARP_CACHE_SIZE 16

void arp_init(void);                                   /* registers the ethertype handler */
/* Resolve an on-link IPv4 address (host byte order) to a MAC, asking the network if it is not cached.
   Waits up to timeout_ms. 0 on success. */
int  arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN], uint32_t timeout_ms);
int  arp_lookup(uint32_t ip, uint8_t mac[ETH_ALEN]);   /* cache only; 0 if known */
int  arp_cache_get(int index, uint32_t *ip, uint8_t mac[ETH_ALEN]); /* for listing; 0 if the slot is used */
void arp_cache_clear(void);

#endif
