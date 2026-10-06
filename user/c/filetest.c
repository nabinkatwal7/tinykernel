/* File I/O through the system calls: write, read back, append, truncate, read-only protection. */
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
	char buf[64];
	int fd, n;

	fd = open("filetest.txt", O_WRONLY | O_CREAT | O_TRUNC);
	if (fd == -6) {
		puts("filetest: no formatted disk - run 'format' first");
		return 1;
	}
	check(fd >= 3, "create");
	check(write(fd, "hello ", 6) == 6, "first write");
	check(write(fd, "world\n", 6) == 6, "second write appends at the cursor");
	check(close(fd) == 0, "close flushes to disk");

	fd = open("filetest.txt", O_RDONLY);
	check(fd >= 3, "reopen for reading");
	memset(buf, 0, sizeof buf);
	n = read(fd, buf, 5);
	check(n == 5 && !memcmp(buf, "hello", 5), "read the first 5 bytes");
	n = read(fd, buf, sizeof buf);
	check(n == 7 && !memcmp(buf, " world\n", 7), "read the rest");
	check(read(fd, buf, sizeof buf) == 0, "EOF returns 0");
	check(write(fd, "x", 1) < 0, "a read-only descriptor cannot be written");
	close(fd);

	fd = open("filetest.txt", O_WRONLY | O_APPEND);
	check(write(fd, "more\n", 5) == 5, "append");
	close(fd);
	fd = open("filetest.txt", O_RDONLY);
	n = read(fd, buf, sizeof buf);
	check(n == 17 && !memcmp(buf + 12, "more\n", 5), "appended data follows the old data");
	close(fd);

	fd = open("filetest.txt", O_WRONLY | O_TRUNC);
	close(fd);
	fd = open("filetest.txt", O_RDONLY);
	check(read(fd, buf, sizeof buf) == 0, "truncate empties the file");
	close(fd);

	write(1, "stdout works via write(1)\n", 26);
	if (!fails)
		puts("filetest: all checks passed");
	return fails;
}
