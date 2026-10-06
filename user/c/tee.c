/* tee [-a] FILE: copy standard input to standard output and to FILE. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

int main(int argc, char **argv)
{
	char buf[256];
	int fd = -1, n, a = 1, flags = O_WRONLY | O_CREAT | O_TRUNC;

	if (a < argc && !strcmp(argv[a], "-a")) {
		flags = O_WRONLY | O_CREAT | O_APPEND;
		a++;
	}
	if (a < argc) {
		fd = open(argv[a], flags);
		if (fd < 0) {
			printf("tee: cannot open %s\n", argv[a]);
			return 1;
		}
	}
	while ((n = read(0, buf, sizeof buf)) > 0) {
		write(1, buf, n);
		if (fd >= 0)
			write(fd, buf, n);
	}
	if (fd >= 0)
		close(fd);
	return 0;
}
