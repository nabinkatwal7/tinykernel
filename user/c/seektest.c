/* lseek(): repositioning reads and writes, extending past the end, SEEK_CUR / SEEK_END. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

static int fails;

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		fails++;
	}
}

int main(void)
{
	char buf[32];
	int fd, n;

	fd = open("seektest.dat", O_RDWR | O_CREAT | O_TRUNC);
	if (fd == -6) {
		puts("seektest: no formatted disk - run 'format' first");
		return 1;
	}
	write(fd, "0123456789", 10);
	check(lseek(fd, 0, SEEK_CUR) == 10, "position after writing 10 bytes");
	check(lseek(fd, 0, SEEK_SET) == 0, "rewind");
	n = read(fd, buf, 4);
	check(n == 4 && !memcmp(buf, "0123", 4), "read from the start");
	check(lseek(fd, 2, SEEK_CUR) == 6, "skip forward with SEEK_CUR");
	n = read(fd, buf, 2);
	check(n == 2 && !memcmp(buf, "67", 2), "read after the skip");
	check(lseek(fd, -3, SEEK_END) == 7, "SEEK_END relative to the size");
	n = read(fd, buf, 10);
	check(n == 3 && !memcmp(buf, "789", 3), "read the last three bytes");
	check(lseek(fd, -1, SEEK_SET) < 0, "a negative position is refused");

	lseek(fd, 3, SEEK_SET);
	write(fd, "XY", 2);
	lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, 10);
	check(n == 10 && !memcmp(buf, "012XY56789", 10), "overwrite in the middle");

	check(lseek(fd, 14, SEEK_SET) == 14, "seek past the end is allowed");
	write(fd, "!", 1);
	check(lseek(fd, 0, SEEK_END) == 15, "the file grew to 15 bytes");
	lseek(fd, 10, SEEK_SET);
	n = read(fd, buf, 5);
	check(n == 5 && buf[0] == 0 && buf[3] == 0 && buf[4] == '!', "the gap reads as zeros");
	close(fd);

	check(lseek(1, 0, SEEK_SET) < 0, "the console is not seekable");
	if (!fails)
		puts("seektest: all checks passed");
	return fails;
}
