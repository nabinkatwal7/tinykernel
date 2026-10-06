#include "ntp.h"

#include "clock.h"
#include "console.h"
#include "dns.h"
#include "ip.h"
#include "kstring.h"
#include "net.h"
#include "rtc.h"
#include "timer.h"
#include "udp.h"

/*
 * SNTP client (RFC 4330): one request, one reply. The packet is 48 bytes; the first byte carries version 3 and
 * mode 3 (client), the server answers with its transmit time in bytes 40-47 as seconds since 1900.
 */
#define NTP_PORT 123
#define NTP_UNIX_OFFSET 2208988800u /* seconds between 1900 and 1970 */

/* Asks 'server' for the time; returns 0 and the unix time (seconds), or a negative error. */
int ntp_query(uint32_t server, uint32_t *unix_time, uint32_t timeout_ms)
{
	uint8_t pkt[48], reply[128];
	uint32_t src, secs;
	uint16_t sport;
	int sock, n, attempt;

	sock = udp_open(0);
	if (sock < 0)
		return -1;
	for (attempt = 0; attempt < 3; attempt++) {
		memset(pkt, 0, sizeof pkt);
		pkt[0] = (3 << 3) | 3; /* version 3, client mode */
		udp_sendto(sock, server, NTP_PORT, pkt, sizeof pkt);
		n = udp_recvfrom(sock, reply, sizeof reply, &src, &sport, timeout_ms / 3);
		if (n >= 48 && (reply[0] & 7) == 4 && reply[1] != 0) { /* a server reply with a valid stratum */
			secs = ((uint32_t)reply[40] << 24) | ((uint32_t)reply[41] << 16) | ((uint32_t)reply[42] << 8) | reply[43];
			if (secs < NTP_UNIX_OFFSET)
				break; /* the date is before 1970: not a real time */
			*unix_time = secs - NTP_UNIX_OFFSET;
			udp_close(sock);
			return 0;
		}
	}
	udp_close(sock);
	return -1;
}

static void print_time(const char *label, uint32_t t)
{
	struct rtc_time r;

	rtc_from_unix(t, &r);
	console_printf("%s %04d-%02d-%02d %02d:%02d:%02d UTC\n", label, r.year, r.month, r.day, r.hour, r.minute, r.second);
}

/* ntpdate [-q] [SERVER] : fetch the time and set the clock (-q: only show the difference) */
int cmd_ntpdate(int argc, char **argv)
{
	const char *host = "pool.ntp.org";
	uint32_t server, t, local;
	struct timespec ts;
	int query_only = 0, a = 1, rc;
	char s[16];

	if (a < argc && !kstrcmp(argv[a], "-q")) {
		query_only = 1;
		a++;
	}
	if (a < argc)
		host = argv[a];
	if (!netif.up || !netif.ip) {
		console_write("ntpdate: no network (run dhcp)\n");
		return 1;
	}
	rc = dns_resolve(host, &server);
	if (rc) {
		console_printf("ntpdate: %s: %s\n", host, dns_strerror(rc));
		return 1;
	}
	if (ntp_query(server, &t, 3000)) {
		console_printf("ntpdate: no answer from %s (%s)\n", host, ip_str(server, s));
		return 1;
	}
	clock_gettime(CLOCK_REALTIME, &ts);
	local = ts.tv_sec;
	console_printf("server %s (%s)\n", host, ip_str(server, s));
	print_time("server time:", t);
	print_time("local time: ", local);
	console_printf("offset: %d second(s)\n", (int)(t - local));
	if (!query_only) {
		clock_set_realtime(t);
		console_write("clock set\n");
	}
	return 0;
}
