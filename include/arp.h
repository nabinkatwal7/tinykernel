#ifndef ARP_H
#define ARP_H

#include <stdint.h>

#include "net.h"

#define ARP_CACHE_SIZE 16

void arp_init(void);                                   /* registers the ethertype handler */
/* Resolve an on-link IPv4 address (host byte order) to a MAC, asking the network if it is not cached.
   Waits up to timeout_ms. 0 on success. */
int  arp_resolve(uint32_t ip, uint8_t mac[ETH_ALEN], uint32_t timeout_ms);
void arp_learn(uint32_t ip, const uint8_t mac[ETH_ALEN]);  /* remember a neighbour seen on the wire */
int  arp_lookup(uint32_t ip, uint8_t mac[ETH_ALEN]);   /* cache only; 0 if known */
void arp_set_ttl(uint32_t seconds);   /* how long a neighbour is trusted without hearing from it (default 60) */
uint32_t arp_get_ttl(void);
uint32_t arp_expired_count(void);    /* entries dropped because they aged out */
int  arp_cache_age(int index);       /* seconds since the entry was last confirmed, or -1 */
int  arp_cache_get(int index, uint32_t *ip, uint8_t mac[ETH_ALEN]); /* for listing; 0 if the slot is used */
void arp_cache_clear(void);

#endif
