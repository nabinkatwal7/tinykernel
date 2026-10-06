/* fork() with copy-on-write address spaces: the child's changes stay private; exit codes come back via waitpid. */
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "usys.h"

static int fails;
static int counter = 100;     /* in .data: shared copy-on-write after fork */

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		fails++;
	}
}

int main(void)
{
	char *heap = malloc(64);
	int pid, status = -1, pid2;

	strcpy(heap, "parent heap");
	pid = fork();
	if (pid < 0) {
		puts("forkprog: fork failed");
		return 1;
	}
	if (pid == 0) { /* child */
		counter += 5;
		strcpy(heap, "child heap!");
		printf("[child ] counter=%d heap='%s'\n", counter, heap);
		sleep_ms(50);
		return 7;
	}
	pid2 = waitpid(pid, &status);
	printf("[parent] child %d finished with status %d\n", pid, status);
	check(pid2 == 0 && status == 7, "waitpid returns the child's exit code");
	check(counter == 100, "the child's write to a global did not reach the parent");
	check(!strcmp(heap, "parent heap"), "the child's write to the heap did not reach the parent");
	check(waitpid(pid, &status) == -1, "a second waitpid on the same child fails");
	counter += 1;
	check(counter == 101, "the parent can still write its own pages");
	if (!fails)
		puts("forkprog: all checks passed");
	return fails;
}
