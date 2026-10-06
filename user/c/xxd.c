/* xxd: a hex dump in xxd's layout (offset, eight groups of two bytes, text). Reads a file, or standard input. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

/* Fills buf with up to n bytes (short only at end of input). */
static int fill(int fd, unsigned char *buf, int n)
{
	int got = 0, r;

	while (got < n && (r = read(fd, buf + got, n - got)) > 0)
		got += r;
	return got;
}

int main(int argc, char **argv)
{
	unsigned char row[16];
	int fd = 0, n, i;
	unsigned off = 0;

	if (argc > 1) {
		fd = open(argv[1], O_RDONLY);
		if (fd < 0) {
			printf("xxd: cannot open %s\n", argv[1]);
			return 1;
		}
	}
	while ((n = fill(fd, row, 16)) > 0) {
		printf("%08x: ", off);
		for (i = 0; i < 16; i++) {
			if (i < n)
				printf("%02x", row[i]);
			else
				printf("  ");
			if (i & 1)
				printf(" ");
		}
		printf(" ");
		for (i = 0; i < n; i++)
			printf("%c", row[i] >= 32 && row[i] < 127 ? row[i] : '.');
		printf("\n");
		off += (unsigned)n;
		if (n < 16)
			break;
	}
	if (fd > 2)
		close(fd);
	return 0;
}
