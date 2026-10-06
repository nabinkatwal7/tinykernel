/* httpget URL [-i]: fetch a URL with HTTP/1.0 and print the body (with -i, the status line first). */
#include "uhttp.h"

int main(int argc, char **argv)
{
	struct http_url u;
	char rest[1024], buf[512];
	int status, length, restlen, fd, n, show = argc > 2 && !strcmp(argv[2], "-i");

	if (argc < 2 || http_parse_url(argv[1], &u)) {
		puts("usage: httpget http://host[:port]/path [-i]");
		return 1;
	}
	fd = http_get(&u, &status, &length, rest, sizeof rest, &restlen);
	if (fd < 0) {
		printf("httpget: %s\n", fd == -1 ? "cannot resolve the host" : fd == -2 ? "cannot connect" : "no valid response");
		return 1;
	}
	if (show)
		printf("HTTP %d, %d bytes announced\n", status, length);
	write(1, rest, restlen);
	while ((n = recv(fd, buf, sizeof buf)) > 0)
		write(1, buf, n);
	close(fd);
	return status >= 200 && status < 300 ? 0 : 1;
}
