/* Sieve of Eratosthenes: a normal C program using printf and a static array. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

#define LIMIT 200

static char composite[LIMIT + 1]; /* in .bss: the ELF loader must zero it */

int main(int argc, char **argv)
{
	int limit = argc > 1 ? atoi(argv[1]) : 100, i, j, count = 0;

	if (limit < 2 || limit > LIMIT) {
		printf("usage: primes [2-%d]\n", LIMIT);
		return 1;
	}
	for (i = 2; i <= limit; i++) {
		if (composite[i])
			continue;
		printf("%d ", i);
		count++;
		for (j = i * i; j <= limit; j += i)
			composite[j] = 1;
	}
	printf("\n%d primes up to %d (took %d ticks)\n", count, limit, ticks());
	return 0;
}
