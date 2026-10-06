/* Demand-paged stack: use `depth` KiB of stack. Up to 15 KiB works (pages appear on first touch);
   going past the 16 KiB stack runs into the guard page and the program is killed. */
#include "stdio.h"
#include "stdlib.h"
#include "usys.h"

static int use_stack(int kib)
{
	volatile char block[1024];
	int i;

	for (i = 0; i < 1024; i += 256)
		block[i] = (char)kib;
	if (kib > 1)
		return use_stack(kib - 1) + block[0];
	return block[0];
}

int main(int argc, char **argv)
{
	int depth = argc > 1 ? atoi(argv[1]) : 8;
	int sum;

	printf("using about %d KiB of stack...\n", depth);
	sum = use_stack(depth);
	printf("survived, checksum %d\n", sum);
	return 0;
}
