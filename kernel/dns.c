#include "dns.h"

#include "console.h"
#include "ip.h"
#include "kstring.h"
#include "net.h"
#include "timer.h"
#include "udp.h"

/*
 * A small DNS stub resolver: one A-record question over UDP to the DHCP-supplied server (or the one the caller
 * names), with the answer parsed out of the response (compressed names included) and cached until its TTL runs out.
 */
#define DNS_PORT   53
#define CACHE_SIZE 8

static struct {
	char name[64];
	uint32_t ip, expires; /* expires: tick count */
	int used;
} cache[CACHE_SIZE];

static uint16_t next_id = 0x4242;
static struct dns_stats stats;

void dns_get_stats(struct dns_stats *out)
{
	*out = stats;
}

/* "www.example.com" -> 3www7example3com0; returns the encoded length, or -1. */
static int encode_name(const char *name, uint8_t *out, int cap)
{
	int o = 0;

	while (*name) {
		const char *dot = name;
		int n = 0;

		while (*dot && *dot != '.')
			dot++, n++;
		if (n == 0 || n > 63 || o + n + 2 > cap)
			return -1;
		out[o++] = (uint8_t)n;
		memcpy(out + o, name, (size_t)n);
		o += n;
		name = *dot ? dot + 1 : dot;
	}
	if (o + 1 > cap)
		return -1;
	out[o++] = 0;
	return o;
}

/* Skip a (possibly compressed) name starting at off; returns the offset after it, or -1. */
static int skip_name(const uint8_t *msg, int len, int off)
{
	while (off < len) {
		uint8_t l = msg[off];

		if (l == 0)
			return off + 1;
		if ((l & 0xC0) == 0xC0)
			return off + 2 <= len ? off + 2 : -1; /* a pointer ends the name */
		off += 1 + l;
	}
	return -1;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* Pull the first A record out of a response to our question. Returns 0, or a DNS_E* code. */
static int parse_response(const uint8_t *msg, int len, uint16_t id, uint32_t *ip, uint32_t *ttl)
{
	int qd, an, off, i, rcode;

	if (len < 12 || get16(msg) != id || !(msg[2] & 0x80))
		return DNS_ERR_BAD;
	rcode = msg[3] & 0x0F;
	if (rcode == 3)
		return DNS_ERR_NXDOMAIN;
	if (rcode)
		return DNS_ERR_SERVER;
	qd = get16(msg + 4);
	an = get16(msg + 6);
	off = 12;
	for (i = 0; i < qd; i++) {
		off = skip_name(msg, len, off);
		if (off < 0 || off + 4 > len)
			return DNS_ERR_BAD;
		off += 4; /* type and class */
	}
	for (i = 0; i < an; i++) {
		uint16_t type, rdlen;

		off = skip_name(msg, len, off);
		if (off < 0 || off + 10 > len)
			return DNS_ERR_BAD;
		type = get16(msg + off);
		*ttl = ((uint32_t)get16(msg + off + 4) << 16) | get16(msg + off + 6);
		rdlen = get16(msg + off + 8);
		off += 10;
		if (off + rdlen > len)
			return DNS_ERR_BAD;
		if (type == 1 && rdlen == 4) { /* A record */
			*ip = ((uint32_t)msg[off] << 24) | ((uint32_t)msg[off + 1] << 16) | ((uint32_t)msg[off + 2] << 8) | msg[off + 3];
			return 0;
		}
		off += rdlen; /* CNAME and friends: keep looking */
	}
	return DNS_ERR_NOANSWER;
}

const char *dns_strerror(int err)
{
	switch (err) {
	case 0: return "ok";
	case DNS_ERR_TIMEOUT: return "no answer from the DNS server";
	case DNS_ERR_NXDOMAIN: return "no such host";
	case DNS_ERR_SERVER: return "the DNS server reported an error";
	case DNS_ERR_BAD: return "malformed DNS response";
	case DNS_ERR_NOANSWER: return "the name has no IPv4 address";
	case DNS_ERR_NOSERVER: return "no DNS server configured (is DHCP done?)";
	default: return "DNS error";
	}
}

void dns_cache_clear(void)
{
	memset(cache, 0, sizeof cache);
}

int dns_cache_get(int i, char name[64], uint32_t *ip, uint32_t *ttl_left)
{
	uint32_t now = timer_ticks();

	if (i < 0 || i >= CACHE_SIZE || !cache[i].used || (int32_t)(cache[i].expires - now) <= 0)
		return -1;
	kstrlcpy(name, cache[i].name, 64);
	*ip = cache[i].ip;
	*ttl_left = (cache[i].expires - now) / timer_hz();
	return 0;
}

static void cache_put(const char *name, uint32_t ip, uint32_t ttl)
{
	int i, victim = 0;

	if (ttl > 3600)
		ttl = 3600;
	for (i = 0; i < CACHE_SIZE; i++) {
		if (cache[i].used && !kstrcmp(cache[i].name, name)) {
			victim = i;
			break;
		}
		if (!cache[i].used)
			victim = i;
	}
	kstrlcpy(cache[victim].name, name, sizeof cache[victim].name);
	cache[victim].ip = ip;
	cache[victim].expires = timer_ticks() + ttl * timer_hz();
	cache[victim].used = 1;
}

int dns_query(const char *name, uint32_t server, uint32_t *ip, uint32_t timeout_ms)
{
	uint8_t q[300], r[512];
	uint16_t id = next_id++;
	int n = 12, enc, sock, attempt, rc = DNS_ERR_TIMEOUT;
	uint32_t ttl = 60, src;
	uint16_t sport;

	if (!server)
		return DNS_ERR_NOSERVER;
	memset(q, 0, sizeof q);
	q[0] = (uint8_t)(id >> 8);
	q[1] = (uint8_t)id;
	q[2] = 0x01; /* recursion desired */
	q[5] = 1;    /* one question */
	enc = encode_name(name, q + n, (int)sizeof q - n - 4);
	if (enc < 0)
		return DNS_ERR_BAD;
	n += enc;
	q[n++] = 0;
	q[n++] = 1; /* type A */
	q[n++] = 0;
	q[n++] = 1; /* class IN */
	sock = udp_open(0);
	if (sock < 0)
		return DNS_ERR_TIMEOUT;
	for (attempt = 0; attempt < 3; attempt++) {
		int got;

		stats.queries++;
		udp_sendto(sock, server, DNS_PORT, q, (uint16_t)n);
		got = udp_recvfrom(sock, r, sizeof r, &src, &sport, timeout_ms / 3);
		if (got < 0)
			continue; /* timed out: ask again */
		rc = parse_response(r, got, id, ip, &ttl);
		if (rc != DNS_ERR_BAD)
			break; /* not a reply to us (BAD) means keep waiting: loop asks again */
	}
	udp_close(sock);
	if (!rc) {
		stats.answers++;
		cache_put(name, *ip, ttl);
	} else {
		stats.failures++;
	}
	return rc;
}

int dns_resolve(const char *name, uint32_t *ip)
{
	uint32_t now = timer_ticks();
	int i;

	if (!ip_parse(name, ip)) /* already a dotted quad */
		return 0;
	if (!kstrcmp(name, "localhost")) {
		*ip = IP4(127, 0, 0, 1);
		return 0;
	}
	for (i = 0; i < CACHE_SIZE; i++) {
		if (cache[i].used && !kstrcmp(cache[i].name, name) && (int32_t)(cache[i].expires - now) > 0) {
			*ip = cache[i].ip;
			stats.cache_hits++;
			return 0;
		}
	}
	return dns_query(name, netif.dns, ip, 3000);
}

/* nslookup NAME [SERVER] : resolve a name (always asking the network); nslookup -c : show the cache; nslookup -s : statistics */
int cmd_nslookup(int argc, char **argv)
{
	uint32_t ip, server = netif.dns, ttl;
	char s[16], name[64];
	int rc, i;

	if (argc == 2 && !kstrcmp(argv[1], "-c")) {
		int n = 0;

		for (i = 0; i < CACHE_SIZE; i++) {
			if (!dns_cache_get(i, name, &ip, &ttl)) {
				console_printf("  %-30s %-15s ttl %us\n", name, ip_str(ip, s), ttl);
				n++;
			}
		}
		if (!n)
			console_write("  (cache empty)\n");
		return 0;
	}
	if (argc == 2 && !kstrcmp(argv[1], "-s")) {
		struct dns_stats st;

		dns_get_stats(&st);
		console_printf("dns: %u queries, %u answers, %u failures, %u cache hits\n", st.queries, st.answers, st.failures, st.cache_hits);
		return 0;
	}
	if (argc < 2 || argc > 3 || (argc == 3 && ip_parse(argv[2], &server))) {
		console_write("usage: nslookup NAME [SERVER] | nslookup -c | nslookup -s\n");
		return 1;
	}
	if (!ip_parse(argv[1], &ip)) { /* a number: nothing to ask */
		console_printf("%s is already an address\n", argv[1]);
		return 0;
	}
	console_printf("Server:  %s\n", server ? ip_str(server, s) : "(none)");
	rc = dns_query(argv[1], server, &ip, 4000);
	if (rc) {
		console_printf("** can't find %s: %s\n", argv[1], dns_strerror(rc));
		return 1;
	}
	console_printf("Name:    %s\nAddress: %s\n", argv[1], ip_str(ip, s));
	return 0;
}
