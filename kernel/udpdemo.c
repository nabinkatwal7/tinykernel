#include "udpdemo.h"

#include "console.h"
#include "ip.h"
#include "kprintf.h"
#include "kstring.h"
#include "keyboard.h"
#include "net.h"
#include "shell.h"
#include "timer.h"
#include "udp.h"

/*
 * Two small UDP programs built on the kernel socket interface (udp.h):
 *   udpecho [PORT [COUNT]]            answers every datagram with the same bytes (default port 7, the echo service)
 *   udpchat LOCAL_PORT IP PORT        a two-person chat: lines typed here go to IP:PORT, lines arriving on LOCAL_PORT are
 *                                     printed; "/quit" leaves
 */
int cmd_udpecho(int argc, char **argv)
{
	uint32_t port = 7, limit = 0, served = 0, src;
	uint16_t sport;
	uint8_t buf[UDP_MAX_PAYLOAD];
	char s[16];
	int sock, n;

	if ((argc > 1 && (kstrtoul(argv[1], &port) || !port || port > 65535)) || (argc > 2 && kstrtoul(argv[2], &limit))) {
		console_write("usage: udpecho [PORT [COUNT]]\n");
		return 1;
	}
	sock = udp_open((uint16_t)port);
	if (sock < 0) {
		console_write("udpecho: cannot open the port (in use, or no network)\n");
		return 1;
	}
	console_printf("udpecho: listening on udp port %u\n", (uint32_t)port);
	while (!limit || served < limit) {
		n = udp_recvfrom(sock, buf, sizeof buf, &src, &sport, 500);
		if (n < 0) {
			if (shell_interrupted())
				break;
			continue;
		}
		console_printf("udpecho: %d byte(s) from %s:%u\n", n, ip_str(src, s), sport);
		udp_sendto(sock, src, sport, buf, (uint16_t)n);
		served++;
	}
	udp_close(sock);
	console_printf("udpecho: %u datagram(s) echoed\n", served);
	return 0;
}

int cmd_udpchat(int argc, char **argv)
{
	uint32_t local, peer_ip, peer_port, src;
	uint16_t sport;
	char line[200], in[UDP_MAX_PAYLOAD + 1], s[16];
	int sock, len = 0, n;

	if (argc != 4 || kstrtoul(argv[1], &local) || !local || local > 65535 || ip_parse(argv[2], &peer_ip) || kstrtoul(argv[3], &peer_port)
	    || !peer_port || peer_port > 65535) {
		console_write("usage: udpchat LOCAL_PORT PEER_IP PEER_PORT\n");
		return 1;
	}
	sock = udp_open((uint16_t)local);
	if (sock < 0) {
		console_write("udpchat: cannot open the port\n");
		return 1;
	}
	console_printf("chat with %s:%u on local port %u. Type a line and press Enter; /quit leaves.\n", ip_str(peer_ip, s), peer_port,
		       (uint32_t)local);
	for (;;) {
		int k;

		while ((k = keyboard_trygetkey()) >= 0) { /* collect the line being typed */
			if (k == '\n') {
				console_putchar('\n');
				line[len] = '\0';
				len = 0;
				if (!kstrcmp(line, "/quit")) {
					udp_close(sock);
					console_write("chat ended\n");
					return 0;
				}
				if (line[0])
					udp_sendto(sock, peer_ip, (uint16_t)peer_port, line, (uint16_t)kstrlen(line));
			} else if (k == '\b') {
				if (len) {
					len--;
					console_putchar('\b');
				}
			} else if (k >= 32 && k < 127 && len < (int)sizeof line - 1) {
				line[len++] = (char)k;
				console_putchar((char)k);
			}
		}
		n = udp_recvfrom(sock, in, UDP_MAX_PAYLOAD, &src, &sport, 40);
		if (n >= 0) {
			in[n] = '\0';
			console_printf("%s[%s:%u] %s\n", len ? "\n" : "", ip_str(src, s), sport, in);
			if (len) { /* redraw the half-typed line */
				line[len] = '\0';
				console_write(line);
			}
		}
		if (shell_interrupted())
			break;
	}
	udp_close(sock);
	return 0;
}
