/* dirname PATH: everything but the last component of a path. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

int main(int argc, char **argv)
{
	char path[64];
	char *slash;
	int n;

	if (argc < 2) {
		puts("usage: dirname PATH");
		return 1;
	}
	strncpy(path, argv[1], sizeof path - 1);
	path[sizeof path - 1] = '\0';
	n = (int)strlen(path);
	while (n > 1 && path[n - 1] == '/')
		path[--n] = '\0';
	slash = strrchr(path, '/');
	if (!slash) {
		puts(".");
	} else if (slash == path) {
		puts("/");
	} else {
		*slash = '\0';
		puts(path);
	}
	return 0;
}
