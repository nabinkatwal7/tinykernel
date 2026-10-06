/* rev [FILE]: print each line backwards. */
#include "stdio.h"
#include "string.h"
#include "uio.h"

static char text[16384];

int main(int argc, char **argv)
{
	int n, i;
	char *p, *eol;

	n = uio_slurp(argc > 1 ? argv[1] : 0, text, sizeof text);
	if (n < 0) {
		printf("rev: cannot read %s\n", argv[1]);
		return 1;
	}
	for (p = text; p < text + n; p = eol + 1) {
		eol = strchr(p, '\n');
		if (!eol)
			eol = text + n;
		*eol = '\0';
		for (i = (int)strlen(p) - 1; i >= 0; i--)
			printf("%c", p[i]);
		printf("\n");
	}
	return 0;
}
