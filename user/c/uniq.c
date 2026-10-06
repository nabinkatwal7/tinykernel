/* uniq [-c] [FILE]: collapse runs of identical adjacent lines; -c prefixes each with its count. */
#include "stdio.h"
#include "string.h"
#include "uio.h"

static char text[16384];

int main(int argc, char **argv)
{
	int count = 0, a = 1, n;
	char *p, *eol, *prev = 0;
	int run = 0;

	if (a < argc && !strcmp(argv[a], "-c")) {
		count = 1;
		a++;
	}
	n = uio_slurp(a < argc ? argv[a] : 0, text, sizeof text);
	if (n < 0) {
		printf("uniq: cannot read %s\n", argv[a]);
		return 1;
	}
	for (p = text; p < text + n; p = eol + 1) {
		eol = strchr(p, '\n');
		if (!eol)
			eol = text + n;
		*eol = '\0';
		if (prev && !strcmp(prev, p)) {
			run++;
			continue;
		}
		if (prev) {
			if (count)
				printf("%4d ", run);
			puts(prev);
		}
		prev = p;
		run = 1;
	}
	if (prev) {
		if (count)
			printf("%4d ", run);
		puts(prev);
	}
	return 0;
}
