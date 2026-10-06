/*
 * nc: a tiny netcat built on the socket system calls.
 *   nc HOST PORT [TEXT]     TCP client: send TEXT (if given), print the first reply (or everything until the server closes)
 *   nc -l PORT              TCP server: accept one client, print what it sends, answer "got N bytes"
 *   nc -u HOST PORT TEXT    UDP: send one datagram, print the reply
 *   nc -ul PORT             UDP: receive one datagram, print it, answer "ack"
 */
#include "stdio.h"
#include "string.h"
#include "usys.h"

static int parse_ip(const char *s, unsigned *ip)
{
	unsigned v[4] = { 0 }, i = 0;

	for (; *s; s++) {
		if (*s == '.') {
			if (++i > 3)
				return -1;
		} else if (*s >= '0' && *s <= '9') {
			v[i] = v[i] * 10 + (unsigned)(*s - '0');
			if (v[i] > 255)
				return -1;
		} else {
			return -1;
		}
	}
	if (i != 3)
		return -1;
	*ip = IP4(v[0], v[1], v[2], v[3]);
	return 0;
}

static int host_ip(const char *name, unsigned *ip)
{
	return parse_ip(name, ip) == 0 || resolve(name, ip) == 0 ? 0 : -1;
}

static void show(const char *what, const char *buf, int n)
{
	printf("%s %d byte(s): ", what, n);
	write(1, buf, n);
	if (n && buf[n - 1] != '\n')
		printf("\n");
}

int main(int argc, char **argv)
{
	char buf[512];
	unsigned ip, from_ip, from_port;
	int fd, n, udp = 0, listen_mode = 0, a = 1, port;

	if (argc > 1 && argv[1][0] == '-') {
		udp = strchr(argv[1], 'u') != 0;
		listen_mode = strchr(argv[1], 'l') != 0;
		a++;
	}
	if (listen_mode) {
		if (a >= argc) {
			puts("usage: nc -l PORT | nc -ul PORT");
			return 1;
		}
		port = atoi(argv[a]);
		fd = socket(udp ? SOCK_DGRAM : SOCK_STREAM);
		if (fd < 0 || bind(fd, port) < 0 || (!udp && listen(fd) < 0)) {
			puts("nc: cannot bind/listen");
			return 1;
		}
		printf("listening on %s port %d\n", udp ? "udp" : "tcp", port);
		if (udp) {
			n = recvfrom(fd, buf, sizeof buf, &from_ip, &from_port);
			if (n < 0)
				return 1;
			show("received", buf, n);
			sendto(fd, "ack", 3, from_ip, (int)from_port);
		} else {
			int c = accept(fd);

			if (c < 0)
				return 1;
			n = recv(c, buf, sizeof buf);
			if (n > 0) {
				char reply[40];

				show("received", buf, n);
				snprintf(reply, sizeof reply, "got %d bytes\n", n);
				send(c, reply, (int)strlen(reply));
			}
			close(c);
		}
		close(fd);
		return 0;
	}
	if (argc < a + 2) {
		puts("usage: nc HOST PORT [TEXT] | nc -u HOST PORT TEXT | nc -l PORT | nc -ul PORT");
		return 1;
	}
	if (host_ip(argv[a], &ip)) {
		printf("nc: cannot resolve %s\n", argv[a]);
		return 1;
	}
	port = atoi(argv[a + 1]);
	fd = socket(udp ? SOCK_DGRAM : SOCK_STREAM);
	if (fd < 0) {
		puts("nc: no socket");
		return 1;
	}
	if (udp) {
		if (a + 2 >= argc) {
			puts("nc: -u needs TEXT");
			return 1;
		}
		sendto(fd, argv[a + 2], (int)strlen(argv[a + 2]), ip, port);
		n = recvfrom(fd, buf, sizeof buf, 0, 0);
		if (n >= 0)
			show("reply", buf, n);
		close(fd);
		return n < 0;
	}
	if (connect(fd, ip, port) < 0) {
		puts("nc: connection failed");
		return 1;
	}
	if (a + 2 < argc) {
		send(fd, argv[a + 2], (int)strlen(argv[a + 2]));
		n = recv(fd, buf, sizeof buf);
		if (n > 0)
			show("reply", buf, n);
	} else {
		while ((n = recv(fd, buf, sizeof buf)) > 0)
			write(1, buf, n);
	}
	close(fd);
	return 0;
}
