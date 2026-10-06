/* basename PATH [SUFFIX]: the last component of a path, optionally without a suffix. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

int main(int argc, char **argv)
{
	char path[64];
	char *base;
	int n, sl;

	if (argc < 2) {
		puts("usage: basename PATH [SUFFIX]");
		return 1;
	}
	strncpy(path, argv[1], sizeof path - 1);
	path[sizeof path - 1] = '\0';
	n = (int)strlen(path);
	while (n > 1 && path[n - 1] == '/')
		path[--n] = '\0';
	base = strrchr(path, '/');
	base = base && base[1] ? base + 1 : path;
	if (argc > 2) {
		sl = (int)strlen(argv[2]);
		n = (int)strlen(base);
		if (n > sl && !strcmp(base + n - sl, argv[2]))
			base[n - sl] = '\0';
	}
	puts(base);
	return 0;
}
