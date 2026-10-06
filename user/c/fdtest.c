/* File descriptors: std streams, dup/dup2 sharing a cursor, and redirecting stdout into a file. */
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
	int fd, copy, saved, n;

	fd = open("fdtest.txt", O_RDWR | O_CREAT | O_TRUNC);
	if (fd == -6) {
		puts("fdtest: no formatted disk - run 'format' first");
		return 1;
	}
	check(fd == 3, "first open gets descriptor 3 (0-2 are the console)");
	copy = dup(fd);
	check(copy == 4, "dup returns the next free descriptor");
	check(write(fd, "ab", 2) == 2 && write(copy, "cd", 2) == 2, "writes through both descriptors");
	close(fd);
	check(write(copy, "ef", 2) == 2, "the dup keeps the file open and shares the cursor");
	close(copy);

	fd = open("fdtest.txt", O_RDONLY);
	n = read(fd, buf, sizeof buf);
	check(n == 6 && !memcmp(buf, "abcdef", 6), "shared cursor: contents are abcdef, no overwriting");
	close(fd);

	/* redirect stdout into a file, like "cmd > file" */
	saved = dup(1);
	fd = open("fdtest.out", O_WRONLY | O_CREAT | O_TRUNC);
	check(dup2(fd, 1) == 1, "dup2 onto stdout");
	close(fd);
	printf("captured %d\n", 123);
	dup2(saved, 1);
	close(saved);
	puts("stdout restored");

	fd = open("fdtest.out", O_RDONLY);
	memset(buf, 0, sizeof buf);
	n = read(fd, buf, sizeof buf);
	check(n == 13 && !strcmp(buf, "captured 123\n"), "printf output landed in the file");
	close(fd);

	check(dup2(99, 5) < 0, "dup2 of a bad descriptor fails");
	check(read(1, buf, 1) < 0, "stdout cannot be read");

	if (!fails)
		puts("fdtest: all checks passed");
	return fails;
}
