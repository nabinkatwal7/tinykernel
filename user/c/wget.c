/* wget URL [FILE]: download a URL into a file (named after the last part of the path by default). */
#include "uhttp.h"

int main(int argc, char **argv)
{
	struct http_url u;
	char rest[1024], buf[1024], name[40];
	int status, length, restlen, fd, out, n, total = 0, shown = 0;
	const char *slash;

	if (argc < 2 || http_parse_url(argv[1], &u)) {
		puts("usage: wget http://host[:port]/path [file]");
		return 1;
	}
	if (argc > 2) {
		strncpy(name, argv[2], sizeof name - 1);
	} else {
		slash = strrchr(u.path, '/');
		strncpy(name, slash && slash[1] ? slash + 1 : "index.html", sizeof name - 1);
	}
	name[sizeof name - 1] = '\0';
	printf("connecting to %s:%d...\n", u.host, u.port);
	fd = http_get(&u, &status, &length, rest, sizeof rest, &restlen);
	if (fd < 0) {
		printf("wget: %s\n", fd == -1 ? "cannot resolve the host" : fd == -2 ? "cannot connect" : "no valid response");
		return 1;
	}
	if (status != 200) {
		printf("wget: server answered %d\n", status);
		close(fd);
		return 1;
	}
	out = open(name, O_WRONLY | O_CREAT | O_TRUNC);
	if (out < 0) {
		printf("wget: cannot create %s (error %d)\n", name, out);
		close(fd);
		return 1;
	}
	printf("saving to %s (%d bytes)\n", name, length);
	write(out, rest, restlen);
	total = restlen;
	while ((n = recv(fd, buf, sizeof buf)) > 0) {
		write(out, buf, n);
		total += n;
		if (total / 4096 > shown) { /* a dot per 4 KiB */
			shown = total / 4096;
			printf(".");
		}
	}
	close(out);
	close(fd);
	printf("\n%d bytes saved%s\n", total, length >= 0 && total != length ? " (INCOMPLETE)" : "");
	return length >= 0 && total != length;
}
