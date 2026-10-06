/* Shared memory: a forked child's writes to a shared segment are visible to the parent, while its
   writes to ordinary memory are not (copy-on-write). */
#include "stdio.h"
#include "string.h"
#include "usys.h"

static int fails;
static int private_value = 1;

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		fails++;
	}
}

int main(void)
{
	int id = shmget(4242, 4096), status = 0, pid, i;
	volatile int *shared;

	check(id >= 0, "shmget creates a segment");
	check(shmget(4242, 4096) == id, "the same key finds the same segment");
	shared = shmat(id);
	check(shared != 0, "shmat maps it");
	check(shared[0] == 0, "a new segment is zeroed");
	shared[0] = 10;
	pid = fork();
	if (pid == 0) {
		for (i = 0; i < 1000; i++)
			shared[0]++;                /* visible to the parent */
		shared[1] = 77;
		private_value = 99;                 /* NOT visible to the parent */
		return 0;
	}
	waitpid(pid, &status);
	check(shared[0] == 1010, "the parent sees the child's 1000 increments");
	check(shared[1] == 77, "and its other write");
	check(private_value == 1, "ordinary memory stayed private to each process");
	check(shmdt((void *)shared) == 0, "shmdt unmaps");
	if (!fails)
		puts("shmtest: all checks passed");
	return fails;
}
