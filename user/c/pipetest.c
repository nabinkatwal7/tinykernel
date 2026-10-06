/* pipe(): a forked child writes through the pipe, the parent reads until end of file. */
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
	int fds[2], pid, status = -1, n, total = 0, bad = 0, i;
	char buf[200], msg[64];

	check(pipe(fds) == 0, "pipe() succeeds");
	check(fds[0] >= 3 && fds[1] == fds[0] + 1, "two new descriptors");

	/* within one process: bytes come out in order */
	check(write(fds[1], "hello", 5) == 5, "write to the pipe");
	n = read(fds[0], msg, sizeof msg);
	check(n == 5 && !memcmp(msg, "hello", 5), "read what was written");

	pid = fork();
	if (pid == 0) { /* child: 10000 bytes, more than the 4 KiB pipe holds, so it must block */
		for (i = 0; i < 100; i++) {
			memset(buf, 'a' + i % 26, 100);
			if (write(fds[1], buf, 100) != 100)
				return 3;
		}
		close(fds[1]);
		return 5;
	}
	while ((n = read(fds[0], buf, sizeof buf)) > 0) {
		for (i = 0; i < n; i++)
			if (buf[i] != 'a' + ((total + i) / 100) % 26)
				bad++;
		total += n;
	}
	check(total == 10000 && !bad, "all 10000 bytes arrived in order");
	check(n == 0, "end of file after the child closed its end");
	check(waitpid(pid, &status) == 0 && status == 5, "child exit status");
	close(fds[0]);
	check(write(fds[1], "x", 1) < 0, "writing with the read end closed fails");
	close(fds[1]);

	if (!fails)
		puts("pipetest: all checks passed");
	return fails;
}
