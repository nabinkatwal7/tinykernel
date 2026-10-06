/* nl [FILE]: number the lines of a file or standard input. */
#include "stdio.h"
#include "string.h"
#include "uio.h"

static char text[16384];

int main(int argc, char **argv)
{
	int n, line = 0;
	char *p, *eol;

	n = uio_slurp(argc > 1 ? argv[1] : 0, text, sizeof text);
	if (n < 0) {
		printf("nl: cannot read %s\n", argv[1]);
		return 1;
	}
	for (p = text; p < text + n; p = eol + 1) {
		eol = strchr(p, '\n');
		if (!eol)
			eol = text + n;
		*eol = '\0';
		printf("%6d  %s\n", ++line, p);
	}
	return 0;
}
