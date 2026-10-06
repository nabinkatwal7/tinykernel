/* yes [string]: print the string (default "y") forever, or until a write fails (the reader went away) or Ctrl+C. */
#include "stdio.h"
#include "string.h"
#include "usys.h"

int main(int argc, char **argv)
{
	char line[128];
	int i, n = 0;

	for (i = 1; i < argc; i++) {
		int l = (int)strlen(argv[i]);

		if (n + l + 2 >= (int)sizeof line)
			break;
		if (i > 1)
			line[n++] = ' ';
		memcpy(line + n, argv[i], (size_t)l);
		n += l;
	}
	if (!n)
		line[n++] = 'y';
	line[n++] = '\n';
	while (write(1, line, n) == n)
		;
	return 0;
}
