#ifndef UHTTP_H
#define UHTTP_H

#include "stdio.h"
#include "string.h"
#include "usys.h"

/* A very small HTTP/1.0 client: GET a URL and hand back the descriptor positioned at the body. */
struct http_url {
	char host[64];
	char path[128];
	int port;
};

static inline int http_parse_url(const char *url, struct http_url *u)
{
	const char *p = url, *slash;
	int n;

	if (!strncmp(p, "http://", 7))
		p += 7;
	u->port = 80;
	slash = strchr(p, '/');
	n = slash ? (int)(slash - p) : (int)strlen(p);
	if (n <= 0 || n >= (int)sizeof u->host)
		return -1;
	memcpy(u->host, p, (size_t)n);
	u->host[n] = '\0';
	{
		char *colon = strchr(u->host, ':');

		if (colon) {
			u->port = atoi(colon + 1);
			*colon = '\0';
		}
	}
	strncpy(u->path, slash ? slash : "/", sizeof u->path - 1);
	u->path[sizeof u->path - 1] = '\0';
	return u->port > 0 ? 0 : -1;
}

static inline int http_parse_ip(const char *s, unsigned *ip)
{
	unsigned v[4] = { 0 }, i = 0;

	for (; *s; s++) {
		if (*s == '.') {
			if (++i > 3)
				return -1;
		} else if (*s >= '0' && *s <= '9') {
			v[i] = v[i] * 10 + (unsigned)(*s - '0');
		} else {
			return -1;
		}
	}
	if (i != 3)
		return -1;
	*ip = IP4(v[0], v[1], v[2], v[3]);
	return 0;
}

/*
 * Connects, sends the request, reads the status line and headers. On success returns the socket; *status is the HTTP
 * status code, *length the Content-Length (or -1), and the first *restlen bytes of the body that came in with the headers
 * are in rest[]. On failure: -1 (resolve), -2 (connect), -3 (no or bad response).
 */
static inline int http_get(const struct http_url *u, int *status, int *length, char *rest, int cap, int *restlen)
{
	char req[300], buf[1024];
	unsigned ip;
	int fd, n, have = 0;
	char *end;

	if (http_parse_ip(u->host, &ip) && resolve(u->host, &ip))
		return -1;
	fd = socket(SOCK_STREAM);
	if (fd < 0 || connect(fd, ip, u->port) < 0)
		return -2;
	n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: tinyos\r\nConnection: close\r\n\r\n", u->path, u->host);
	send(fd, req, n);
	while (have < (int)sizeof buf - 1) {
		n = recv(fd, buf + have, (int)sizeof buf - 1 - have);
		if (n <= 0)
			break;
		have += n;
		buf[have] = '\0';
		if (strstr(buf, "\r\n\r\n"))
			break;
	}
	end = strstr(buf, "\r\n\r\n");
	if (!end || strncmp(buf, "HTTP/", 5)) {
		close(fd);
		return -3;
	}
	*end = '\0';
	*status = atoi(strchr(buf, ' ') ? strchr(buf, ' ') + 1 : "0");
	*length = -1;
	{
		const char *cl = strstr(buf, "Content-Length:");

		if (!cl)
			cl = strstr(buf, "content-length:");
		if (cl)
			*length = atoi(cl + 15);
	}
	end += 4;
	n = have - (int)(end - buf);
	if (n > cap)
		n = cap;
	memcpy(rest, end, (size_t)n);
	*restlen = n;
	return fd;
}

#endif
