#include "dhcp.h"

#include "ip.h"
#include "klog.h"
#include "kstring.h"
#include "timer.h"
#include "udp.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define MAGIC_COOKIE     0x63825363u

#define OPT_MASK      1
#define OPT_ROUTER    3
#define OPT_DNS       6
#define OPT_REQ_IP    50
#define OPT_LEASE     51
#define OPT_MSG_TYPE  53
#define OPT_SERVER_ID 54
#define OPT_PARAMS    55
#define OPT_END       255

#define DISCOVER 1
#define OFFER    2
#define REQUEST  3
#define ACK      5
#define NAK      6

struct bootp {
	uint8_t op, htype, hlen, hops;
	uint32_t xid;            /* big endian */
	uint16_t secs, flags;
	uint8_t ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
	uint8_t chaddr[16];
	uint8_t sname[64], file[128];
	uint32_t cookie;
	uint8_t options[300];
} __attribute__((packed));

static uint32_t lease;
static uint32_t xid_counter;

uint32_t dhcp_lease_seconds(void)
{
	return lease;
}

static uint32_t get_ip(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int add_opt(uint8_t *o, int pos, uint8_t code, const void *data, uint8_t len)
{
	o[pos++] = code;
	o[pos++] = len;
	memcpy(o + pos, data, len);
	return pos + len;
}

static uint16_t build(struct bootp *b, uint8_t type, uint32_t xid, uint32_t req_ip, uint32_t server)
{
	static const uint8_t params[] = { OPT_MASK, OPT_ROUTER, OPT_DNS, OPT_LEASE };
	int pos = 0;
	uint8_t v[4];

	memset(b, 0, sizeof *b);
	b->op = 1;           /* BOOTREQUEST */
	b->htype = 1;        /* Ethernet */
	b->hlen = 6;
	b->xid = htonl(xid);
	b->flags = htons(0x8000); /* ask the server to broadcast its answer: we have no address yet */
	memcpy(b->chaddr, netif.mac, 6);
	b->cookie = htonl(MAGIC_COOKIE);
	pos = add_opt(b->options, pos, OPT_MSG_TYPE, &type, 1);
	if (type == REQUEST) {
		v[0] = (uint8_t)(req_ip >> 24); v[1] = (uint8_t)(req_ip >> 16);
		v[2] = (uint8_t)(req_ip >> 8);  v[3] = (uint8_t)req_ip;
		pos = add_opt(b->options, pos, OPT_REQ_IP, v, 4);
		v[0] = (uint8_t)(server >> 24); v[1] = (uint8_t)(server >> 16);
		v[2] = (uint8_t)(server >> 8);  v[3] = (uint8_t)server;
		pos = add_opt(b->options, pos, OPT_SERVER_ID, v, 4);
	}
	pos = add_opt(b->options, pos, OPT_PARAMS, params, sizeof params);
	b->options[pos++] = OPT_END;
	return (uint16_t)(offsetof(struct bootp, options) + pos);
}

/* Wait for a reply of the wanted type for our transaction; fill the offered values. */
static int wait_for(int sock, uint32_t xid, uint8_t want, uint32_t timeout_ms, uint32_t *yiaddr, uint32_t *server,
		    uint32_t *mask, uint32_t *router, uint32_t *dns, uint32_t *lease_s)
{
	uint8_t buf[600];
	uint32_t deadline = timer_ticks() + timeout_ms * timer_hz() / 1000;

	for (;;) {
		struct bootp *b = (struct bootp *)buf;
		int n, i, got_type = 0;
		uint32_t left = (int32_t)(deadline - timer_ticks()) > 0 ? (deadline - timer_ticks()) * (1000 / timer_hz()) : 0;

		if (!left)
			return -1;
		n = udp_recvfrom(sock, buf, sizeof buf, 0, 0, left);
		if (n < (int)offsetof(struct bootp, options) || b->op != 2 || ntohl(b->xid) != xid
		    || ntohl(b->cookie) != MAGIC_COOKIE || memcmp(b->chaddr, netif.mac, 6))
			continue;
		*yiaddr = get_ip(b->yiaddr);
		for (i = 0; i < n - (int)offsetof(struct bootp, options);) {
			uint8_t code = b->options[i], len;

			if (code == OPT_END)
				break;
			if (code == 0) { /* pad */
				i++;
				continue;
			}
			len = b->options[i + 1];
			if (i + 2 + len > n - (int)offsetof(struct bootp, options))
				break;
			switch (code) {
			case OPT_MSG_TYPE: got_type = b->options[i + 2]; break;
			case OPT_SERVER_ID: *server = get_ip(&b->options[i + 2]); break;
			case OPT_MASK: *mask = get_ip(&b->options[i + 2]); break;
			case OPT_ROUTER: *router = get_ip(&b->options[i + 2]); break;
			case OPT_DNS: *dns = get_ip(&b->options[i + 2]); break;
			case OPT_LEASE: *lease_s = get_ip(&b->options[i + 2]); break;
			}
			i += 2 + len;
		}
		if (got_type == NAK)
			return -2;
		if (got_type == want)
			return 0;
	}
}

int dhcp_acquire(uint32_t timeout_ms)
{
	static struct bootp pkt;
	uint32_t xid, yiaddr = 0, server = 0, mask = 0, router = 0, dns = 0, lease_s = 0;
	uint32_t old_ip = netif.ip, old_mask = netif.netmask;
	uint16_t len;
	int sock, rc = -2;

	if (!netif.up)
		return -1;
	sock = udp_open(DHCP_CLIENT_PORT);
	if (sock < 0)
		return -1;
	xid = 0x54494E59u + ++xid_counter + timer_ticks(); /* "TINY" + a little noise */
	netif.ip = 0;       /* DHCP packets go out from 0.0.0.0 to 255.255.255.255 */
	netif.netmask = 0;

	len = build(&pkt, DISCOVER, xid, 0, 0);
	udp_sendto(sock, 0xFFFFFFFFu, DHCP_SERVER_PORT, &pkt, len);
	if (wait_for(sock, xid, OFFER, timeout_ms, &yiaddr, &server, &mask, &router, &dns, &lease_s))
		goto fail;

	rc = -3;
	len = build(&pkt, REQUEST, xid, yiaddr, server);
	udp_sendto(sock, 0xFFFFFFFFu, DHCP_SERVER_PORT, &pkt, len);
	if (wait_for(sock, xid, ACK, timeout_ms, &yiaddr, &server, &mask, &router, &dns, &lease_s))
		goto fail;

	netif.ip = yiaddr;
	netif.netmask = mask ? mask : IP4(255, 255, 255, 0);
	netif.gateway = router;
	netif.dns = dns;
	lease = lease_s;
	udp_close(sock);
	klog(LOG_INFO, "dhcp: got %u.%u.%u.%u from server %u.%u.%u.%u, lease %us", yiaddr >> 24, (yiaddr >> 16) & 255,
	     (yiaddr >> 8) & 255, yiaddr & 255, server >> 24, (server >> 16) & 255, (server >> 8) & 255, server & 255,
	     lease_s);
	return 0;
fail:
	netif.ip = old_ip; /* keep whatever configuration we had */
	netif.netmask = old_mask;
	udp_close(sock);
	return rc;
}
