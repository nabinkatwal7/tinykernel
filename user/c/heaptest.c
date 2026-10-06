/* Heap growth by page fault: sbrk only moves the break, pages appear when touched, memory is zeroed,
   shrinking releases pages, and touching past the break is a fault. */
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

int main(int argc, char **argv)
{
	(void)argv;
	char *base = sbrk(0), *p;
	int i, zero = 1;

	if (argc > 1) { /* `heaptest crash`: touch memory beyond the break */
		puts("touching memory past the break...");
		base[64 * 1024] = 1;
		puts("not reached");
		return 0;
	}
	p = sbrk(40 * 1024);
	check(p == base, "sbrk returns the old break");
	for (i = 0; i < 40 * 1024; i++)
		zero &= p[i] == 0;
	check(zero, "fresh heap pages read as zeros");
	memset(p, 0xAB, 40 * 1024);
	check((unsigned char)p[40 * 1024 - 1] == 0xAB, "all of the heap is writable");
	check(sbrk(-30 * 1024) == p + 40 * 1024, "shrinking returns the previous break");
	p = sbrk(30 * 1024);               /* grow again over pages that were released */
	zero = 1;
	for (i = 0; i < 30 * 1024; i++)
		zero &= ((char *)p)[i] == 0;
	check(zero, "pages given back and re-grown come back zeroed");
	check(sbrk(1 << 20) == (void *)-1, "growing beyond the window fails cleanly");
	if (!fails)
		puts("heaptest: all checks passed");
	return fails;
}
