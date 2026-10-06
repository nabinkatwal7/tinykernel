/* flock(): shared and exclusive advisory locks, non-blocking refusals, and a blocked request that is granted later. */
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
	int a, b, pid, status = -1;

	a = open("lock.dat", O_RDWR | O_CREAT);
	if (a == -6) {
		puts("locktest: no formatted disk - run 'format' first");
		return 1;
	}
	b = open("lock.dat", O_RDWR);
	check(a >= 0 && b >= 0, "two descriptors for one file");

	check(flock(a, LOCK_SH) == 0 && flock(b, LOCK_SH | LOCK_NB) == 0, "shared locks coexist");
	check(flock(b, LOCK_EX | LOCK_NB) < 0, "exclusive is refused while another shared lock exists");
	check(flock(a, LOCK_UN) == 0 && flock(b, LOCK_EX | LOCK_NB) == 0, "after unlocking, upgrade to exclusive works");
	check(flock(a, LOCK_SH | LOCK_NB) < 0, "shared is refused under an exclusive lock");
	check(flock(b, LOCK_UN) == 0 && flock(a, LOCK_EX | LOCK_NB) == 0, "exclusive is free again");

	/* a forked child asks for the lock and has to wait until we let go */
	pid = fork();
	if (pid == 0) {
		int c = open("lock.dat", O_RDWR);

		if (flock(c, LOCK_EX | LOCK_NB) >= 0)
			return 2; /* must not be granted yet */
		if (flock(c, LOCK_EX) < 0) /* blocks until the parent unlocks */
			return 3;
		return 0;
	}
	sleep_ms(200);
	check(flock(a, LOCK_UN) == 0, "parent releases the lock");
	check(waitpid(pid, &status) == 0 && status == 0, "the waiting child got the lock afterwards");

	close(a);
	close(b);
	if (!fails)
		puts("locktest: all checks passed");
	return fails;
}
