/* Device files: /dev/zero and /dev/random behave like streams, not like stored files. */
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
	unsigned char a[64], b[64];
	int fd, i, zeros = 1, differ = 0, distinct = 0;

	fd = open("/dev/zero", O_RDONLY);
	check(fd >= 3, "open /dev/zero");
	memset(a, 0xAA, sizeof a);
	check(read(fd, a, sizeof a) == 64, "read returns what was asked for (it never hits EOF)");
	for (i = 0; i < 64; i++)
		zeros &= a[i] == 0;
	check(zeros, "/dev/zero reads zeros");
	check(write(fd, "x", 1) < 0, "read-only open cannot be written");
	close(fd);

	fd = open("/dev/random", O_RDONLY);
	check(fd >= 3, "open /dev/random");
	read(fd, a, sizeof a);
	read(fd, b, sizeof b);
	for (i = 0; i < 64; i++) {
		differ |= a[i] != b[i];
		distinct += a[i] != a[0];
	}
	check(differ, "two reads give different bytes");
	check(distinct > 20, "bytes within a read vary");
	close(fd);

	fd = open("/dev/null", O_RDWR);
	check(fd >= 3, "open /dev/null");
	check(write(fd, "discarded", 9) == 9, "writes to /dev/null succeed");
	check(read(fd, a, sizeof a) == 0, "reads from /dev/null hit EOF at once");
	close(fd);

	fd = open("/dev/console", O_WRONLY);
	check(fd >= 3, "open /dev/console");
	check(write(fd, "written through /dev/console\n", 29) == 29, "console device writes");
	close(fd);

	/* redirect stdout to /dev/null: nothing should appear */
	fd = open("/dev/null", O_WRONLY);
	i = dup(1);
	dup2(fd, 1);
	close(fd);
	puts("THIS MUST NOT BE VISIBLE");
	dup2(i, 1);
	close(i);

	check(open("/dev/nosuchdevice", O_RDONLY) < 0, "unknown device fails");
	if (!fails)
		puts("devtest: all checks passed");
	return fails;
}
