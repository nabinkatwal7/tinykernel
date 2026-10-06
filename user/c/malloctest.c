/* malloc/free/realloc over sbrk: patterns survive, freed memory is reused, huge requests fail. */
#include "stdio.h"
#include "stdlib.h"
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
	char *a = malloc(100), *b = malloc(5000), *c = malloc(10);
	char *p[40];
	int i, intact = 1;

	check(a && b && c, "basic allocations");
	check(((unsigned)a & 7) == 0 && ((unsigned)b & 7) == 0, "8-byte alignment");
	memset(a, 'a', 100);
	memset(b, 'b', 5000);
	memset(c, 'c', 10);
	check(a[99] == 'a' && b[4999] == 'b' && c[9] == 'c', "blocks do not overlap");

	free(b);
	p[0] = malloc(4000);
	check(p[0] == b, "freed block is reused");
	free(p[0]);
	free(a);
	free(c);

	for (i = 0; i < 40; i++) {
		p[i] = malloc(64 + (unsigned)i * 8);
		if (p[i])
			memset(p[i], i + 1, 64);
	}
	for (i = 0; i < 40; i += 2)
		free(p[i]);
	for (i = 1; i < 40; i += 2)
		intact &= p[i] && p[i][0] == i + 1 && p[i][63] == i + 1;
	check(intact, "surviving blocks intact after freeing every other one");
	for (i = 1; i < 40; i += 2)
		free(p[i]);

	a = malloc(8);
	strcpy(a, "grow");
	a = realloc(a, 3000);
	check(a && !strcmp(a, "grow"), "realloc preserves contents");
	free(a);

	check(malloc(10 * 1024 * 1024) == 0, "an impossible request returns NULL");
	check(calloc(4, 4) != 0, "calloc");

	if (!fails)
		puts("malloctest: all checks passed");
	return fails;
}
