#include "netstat.h"

#include "arp.h"
#include "console.h"
#include "dns.h"
#include "icmp.h"
#include "ip.h"
#include "kprintf.h"
#include "kstring.h"
#include "net.h"
#include "tcp.h"
#include "udp.h"

/* netstat [-s | -c | -a] : traffic counters of every layer (-s, the default), the TCP connection table (-c) or both (-a) */
static void connections(void)
{
	int i, n = 0;

	console_write("Proto  Local port  Remote address        State\n");
	for (i = 0; i < TCP_MAX_CONN; i++) {
		uint32_t ip;
		uint16_t rport, lport;
		enum tcp_state st;
		char s[16], remote[24];

		if (tcp_conn_info(i, &ip, &rport, &lport, &st))
			continue;
		if (st == TCP_LISTEN)
			kstrlcpy(remote, "*:*", sizeof remote);
		else
			ksnprintf(remote, sizeof remote, "%s:%u", ip_str(ip, s), rport);
		console_printf("tcp    %-10u  %-20s  %s\n", lport, remote, tcp_state_name(st));
		n++;
	}
	for (i = 0; i < UDP_MAX_SOCKETS; i++) {
		uint16_t port = udp_local_port(i);

		if (port) {
			console_printf("udp    %-10u  %-20s  -\n", port, "*:*");
			n++;
		}
	}
	if (!n)
		console_write("(no open connections or sockets)\n");
}

static void counters(void)
{
	struct ip_stats ip;
	struct udp_stats udp;
	struct icmp_stats icmp;
	struct tcp_stats tcp;
	struct dns_stats dns;
	uint32_t full, depth;
	char ips[16];
	int i, arp = 0;

	ip_get_stats(&ip);
	udp_get_stats(&udp);
	icmp_get_stats(&icmp);
	tcp_get_stats(&tcp);
	dns_get_stats(&dns);
	net_tx_stats(&full, &depth);
	for (i = 0; i < ARP_CACHE_SIZE; i++) {
		uint32_t a;
		uint8_t mac[ETH_ALEN];

		arp += !arp_cache_get(i, &a, mac);
	}
	console_printf("interface: %s, address %s\n", netif.up ? "up" : "down", ip_str(netif.ip, ips));
	console_printf("  frames:  %u received (%u errors, %u dropped), %u sent (%u errors); transmit queue peaked at %u, senders waited %u time(s)\n",
		       netif.rx_frames, netif.rx_errors, netif.rx_dropped, netif.tx_frames, netif.tx_errors, depth, full);
	console_printf("  arp:     %d cached neighbour(s)\n", arp);
	console_printf("  ip:      %u packets received (%u bad, %u not for us), %u sent (%u failed to resolve)\n", ip.rx_packets, ip.rx_bad,
		       ip.rx_not_for_us, ip.tx_packets, ip.tx_arp_fail);
	console_printf("  icmp:    echo requests %u in / %u out, replies %u in / %u out\n", icmp.echo_requests_in, icmp.echo_requests_out,
		       icmp.echo_replies_in, icmp.echo_replies_out);
	console_printf("  udp:     %u received, %u sent, %u without a socket, %u bad checksum, %u dropped\n", udp.rx, udp.tx, udp.rx_no_socket,
		       udp.rx_bad_checksum, udp.rx_dropped);
	console_printf("  tcp:     %u segments received, %u sent, %u retransmitted, %u window probes, %u aborted, %u bad checksum, %u resets sent\n",
		       tcp.segments_received, tcp.segments_sent, tcp.retransmits, tcp.window_probes, tcp.aborted, tcp.bad_checksum, tcp.resets_sent);
	console_printf("  dns:     %u queries, %u answers, %u failures, %u cache hits\n", dns.queries, dns.answers, dns.failures, dns.cache_hits);
}

int cmd_netstat(int argc, char **argv)
{
	int c = argc > 1 && !kstrcmp(argv[1], "-c"), all = argc > 1 && !kstrcmp(argv[1], "-a");

	if (argc > 1 && !c && !all && kstrcmp(argv[1], "-s")) {
		console_write("usage: netstat [-s | -c | -a]\n");
		return 1;
	}
	if (!c)
		counters();
	if (c || all)
		connections();
	return 0;
}
