/* exec(): a failed exec returns -1 and we carry on; a good one replaces this program. */
#include "stdio.h"
#include "usys.h"

int main(int argc, char **argv)

{
	(void)argv;
	char *args[] = { "args", "replaced-by-exec", "42", 0 };

	if (argc > 1) { /* only reachable if exec failed to replace us */
		puts("exectest: still here?!");
		return 99;
	}
	puts("exectest: before exec");
	if (exec("no-such-program", args) != -1)
		puts("FAIL: exec of a missing program should return -1");
	else
		puts("missing program: exec returned -1, still running");
	puts("exectest: now exec'ing 'args'");
	exec("args", args);
	puts("FAIL: exec returned");
	return 1;
}
