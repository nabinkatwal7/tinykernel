/*
 * httpd [PORT [REQUESTS]]: a small web server for the current directory.
 * Serves GET requests for files (/name) and directory listings (/ or /dir/). One request at a time; stops after
 * REQUESTS requests (default: runs until Ctrl+C). Port 8080 by default.
 */
#include "stdio.h"
#include "string.h"
#include "usys.h"

static int send_all(int fd, const char *buf, int len)
{
	return send(fd, buf, len) == len ? 0 : -1;
}

static void respond(int c, int code, const char *reason, const char *type, const char *body, int len)
{
	char head[160];
	int n = snprintf(head, sizeof head, "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", code, reason,
			 type, len);

	send_all(c, head, n);
	if (len)
		send_all(c, body, len);
}

static const char *content_type(const char *path)
{
	const char *dot = strrchr(path, '.');

	if (dot && (!strcmp(dot, ".html") || !strcmp(dot, ".htm")))
		return "text/html";
	return "text/plain";
}

static void serve_dir(int c, const char *path)
{
	static char listing[2048], body[3072];
	int n, len = 0, i = 0;

	n = dirlist(path[0] ? path : ".", listing, sizeof listing - 1);
	if (n < 0) {
		respond(c, 404, "Not Found", "text/plain", "no such directory\n", 18);
		return;
	}
	listing[n] = '\0';
	len = snprintf(body, sizeof body, "<html><body><h1>Index of /%s</h1><ul>\n", path);
	while (i < n && len < (int)sizeof body - 200) {
		char name[40], kind;
		unsigned size;
		int k = 0;

		while (i < n && listing[i] != ' ' && k < (int)sizeof name - 1)
			name[k++] = listing[i++];
		name[k] = '\0';
		size = 0;
		i++;
		while (i < n && listing[i] >= '0' && listing[i] <= '9')
			size = size * 10 + (unsigned)(listing[i++] - '0');
		i++;
		kind = listing[i];
		while (i < n && listing[i] != '\n')
			i++;
		i++;
		len += snprintf(body + len, sizeof body - (size_t)len, "<li><a href=\"/%s%s%s\">%s%s</a> (%u bytes)\n", path, path[0] ? "/" : "",
				name, name, kind == 'd' ? "/" : "", size);
	}
	len += snprintf(body + len, sizeof body - (size_t)len, "</ul></body></html>\n");
	respond(c, 200, "OK", "text/html", body, len);
}

static void serve_file(int c, const char *path)
{
	char buf[1024], head[160];
	int fd = open(path, O_RDONLY), size, n;

	if (fd < 0) {
		respond(c, 404, "Not Found", "text/plain", "not found\n", 10);
		return;
	}
	size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	n = snprintf(head, sizeof head, "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", content_type(path), size);
	send_all(c, head, n);
	while ((n = read(fd, buf, sizeof buf)) > 0)
		if (send_all(c, buf, n))
			break;
	close(fd);
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 8080, limit = argc > 2 ? atoi(argv[2]) : 0, served = 0;
	int s = socket(SOCK_STREAM);

	if (s < 0 || bind(s, port) < 0 || listen(s) < 0) {
		puts("httpd: cannot listen");
		return 1;
	}
	printf("httpd: serving the current directory on port %d\n", port);
	while (!limit || served < limit) {
		char req[600], path[128];
		int c = accept(s), n, have = 0, isdir;

		if (c < 0)
			break; /* Ctrl+C */
		while (have < (int)sizeof req - 1) {
			n = recv(c, req + have, (int)sizeof req - 1 - have);
			if (n <= 0)
				break;
			have += n;
			req[have] = '\0';
			if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n"))
				break;
		}
		req[have] = '\0';
		served++;
		if (strncmp(req, "GET /", 5)) {
			respond(c, 405, "Method Not Allowed", "text/plain", "only GET\n", 9);
		} else {
			char *end = strchr(req + 5, ' ');

			if (end)
				*end = '\0';
			strncpy(path, req + 5, sizeof path - 1);
			path[sizeof path - 1] = '\0';
			n = (int)strlen(path);
			isdir = n == 0 || path[n - 1] == '/';
			if (n && isdir)
				path[n - 1] = '\0';
			printf("GET /%s\n", path);
			if (strstr(path, ".."))
				respond(c, 403, "Forbidden", "text/plain", "no\n", 3);
			else if (isdir)
				serve_dir(c, path);
			else
				serve_file(c, path);
		}
		close(c);
	}
	close(s);
	printf("httpd: %d request(s) served\n", served);
	return 0;
}
