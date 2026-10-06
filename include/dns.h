#ifndef DNS_H
#define DNS_H

#include <stdint.h>

/* A tiny DNS stub resolver (A records over UDP) with a small TTL cache. */
#define DNS_ERR_TIMEOUT  (-1)
#define DNS_ERR_NXDOMAIN (-2)
#define DNS_ERR_SERVER   (-3)
#define DNS_ERR_BAD      (-4)
#define DNS_ERR_NOANSWER (-5)
#define DNS_ERR_NOSERVER (-6)

/* Name or dotted quad to an IPv4 address (host byte order): numbers pass through, "localhost" is built in,
   cached answers are reused, everything else asks the DHCP-supplied DNS server. 0 or a DNS_ERR_* code. */
int  dns_resolve(const char *name, uint32_t *ip);
int  dns_query(const char *name, uint32_t server, uint32_t *ip, uint32_t timeout_ms);   /* always asks 'server' */
const char *dns_strerror(int err);
void dns_cache_clear(void);
int  dns_cache_get(int index, char name[64], uint32_t *ip, uint32_t *ttl_left); /* for listing; 0 if the slot holds a live entry */

struct dns_stats {
	uint32_t queries, answers, failures, cache_hits;
};
void dns_get_stats(struct dns_stats *out);

int  cmd_nslookup(int argc, char **argv);   /* nslookup NAME [SERVER] | nslookup -c (show/clear the cache) */

#endif
