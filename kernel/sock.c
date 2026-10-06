#include "sock.h"

#include "dns.h"
#include "fs.h"
#include "kmalloc.h"
#include "kstring.h"
#include "net.h"
#include "tcp.h"
#include "udp.h"
#include "user.h"

/*
 * BSD-style sockets over TCP and UDP. A socket is a descriptor (see file.c), so read(), write() and close() work on
 * it as on a file. Blocking calls wake up every second to notice Ctrl+C.
 */
struct sock {
	int type;            /* SOCK_STREAM or SOCK_DGRAM */
	int handle;          /* tcp connection / listener id, or udp socket id; -1 until there is one */
	uint16_t port;       /* port given to bind() */
	int listening;
	int connected;
	uint32_t peer_ip;
	uint16_t peer_port;
};

struct sock *sock_new(int type)
{
	struct sock *s;

	if (type != SOCK_STREAM && type != SOCK_DGRAM)
		return 0;
	s = kcalloc(1, sizeof *s);
	if (s) {
		s->type = type;
		s->handle = -1;
	}
	return s;
}

void sock_free(struct sock *s)
{
	if (s->handle >= 0) {
		if (s->type == SOCK_STREAM)
			tcp_close(s->handle);
		else
			udp_close(s->handle);
	}
	kfree(s);
}

int sock_bind(struct sock *s, uint16_t port)
{
	if (s->handle >= 0 || s->port)
		return FS_EINVAL;
	if (s->type == SOCK_DGRAM) {
		s->handle = udp_open(port);
		if (s->handle < 0)
			return FS_EEXIST; /* port in use */
		s->port = udp_local_port(s->handle);
		return FS_OK;
	}
	s->port = port; /* a stream socket binds when it starts listening */
	return FS_OK;
}

int sock_listen(struct sock *s)
{
	if (s->type != SOCK_STREAM || s->handle >= 0 || !s->port)
		return FS_EINVAL;
	s->handle = tcp_listen(s->port);
	if (s->handle < 0)
		return FS_EEXIST;
	s->listening = 1;
	return FS_OK;
}

/* Waits for a client; blocks until one comes (or Ctrl+C). The new connection is returned as a socket. */
struct sock *sock_accept(struct sock *s, int *err)
{
	struct sock *n;
	uint32_t ip;
	uint16_t port;
	int c;

	*err = FS_EINVAL;
	if (s->type != SOCK_STREAM || !s->listening)
		return 0;
	do {
		c = tcp_accept(s->handle, 1000, &ip, &port);
		if (c < 0 && user_abort_requested()) {
			*err = FS_EIO;
			return 0;
		}
	} while (c < 0);
	n = sock_new(SOCK_STREAM);
	if (!n) {
		tcp_close(c);
		*err = FS_ENOSPC;
		return 0;
	}
	n->handle = c;
	n->connected = 1;
	n->peer_ip = ip;
	n->peer_port = port;
	n->port = s->port;
	return n;
}

int sock_connect(struct sock *s, uint32_t ip, uint16_t port)
{
	int c;

	if (s->handle >= 0)
		return FS_EINVAL;
	if (s->type == SOCK_DGRAM) { /* a datagram socket remembers the default destination */
		s->handle = udp_open(s->port);
		if (s->handle < 0)
			return FS_ENOSPC;
		s->peer_ip = ip;
		s->peer_port = port;
		s->connected = 1;
		return FS_OK;
	}
	c = tcp_connect(ip, port, 5000);
	if (c < 0)
		return c == -2 ? FS_EACCES : c == -1 ? FS_EIO : FS_ENOSPC;
	s->handle = c;
	s->connected = 1;
	s->peer_ip = ip;
	s->peer_port = port;
	return FS_OK;
}

/* Stream: bytes (0 when the peer has closed). Datagram: one datagram from the connected peer or anyone. */
int sock_recv(struct sock *s, void *buf, uint32_t cap, uint32_t *from_ip, uint16_t *from_port)
{
	int n;

	if (s->handle < 0 && s->type == SOCK_DGRAM) {
		s->handle = udp_open(s->port);
		if (s->handle < 0)
			return FS_ENOSPC;
	}
	if (s->handle < 0 || s->listening)
		return FS_EINVAL;
	if (cap > 4000)
		cap = 4000;
	do {
		if (s->type == SOCK_STREAM) {
			n = tcp_recv(s->handle, buf, (uint16_t)cap, 1000);
			if (n >= 0 && from_ip)
				*from_ip = s->peer_ip;
		} else {
			uint32_t ip;
			uint16_t port;

			n = udp_recvfrom(s->handle, buf, (uint16_t)cap, &ip, &port, 1000);
			if (n >= 0 && from_ip)
				*from_ip = ip;
			if (n >= 0 && from_port)
				*from_port = port;
		}
		if (n < 0 && user_abort_requested())
			return FS_EIO;
	} while (n < 0);
	return n;
}

int sock_send(struct sock *s, const void *buf, uint32_t len, uint32_t ip, uint16_t port)
{
	if (s->type == SOCK_STREAM) {
		if (s->handle < 0 || s->listening || !s->connected)
			return FS_EINVAL;
		return tcp_send(s->handle, buf, len, 10000) ? FS_EIO : (int)len;
	}
	if (s->handle < 0) {
		s->handle = udp_open(s->port);
		if (s->handle < 0)
			return FS_ENOSPC;
	}
	if (!ip && s->connected) {
		ip = s->peer_ip;
		port = s->peer_port;
	}
	if (!ip || len > UDP_MAX_PAYLOAD)
		return FS_EINVAL;
	return udp_sendto(s->handle, ip, port, buf, (uint16_t)len) ? FS_EIO : (int)len;
}

int sock_resolve(const char *name, uint32_t *ip)
{
	return dns_resolve(name, ip) ? FS_ENOENT : FS_OK;
}
