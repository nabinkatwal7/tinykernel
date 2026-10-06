/* sleep SECONDS: pause for a (possibly fractional) number of seconds, e.g. "sleep 0.5". */
#include "stdio.h"
#include "string.h"
#include "usys.h"

int main(int argc, char **argv)
{
	int ms = 0, frac = 0, scale = 100;
	const char *p;

	if (argc != 2) {
		puts("usage: sleep SECONDS");
		return 1;
	}
	for (p = argv[1]; *p >= '0' && *p <= '9'; p++)
		ms = ms * 10 + (*p - '0');
	ms *= 1000;
	if (*p == '.') {
		for (p++; *p >= '0' && *p <= '9' && scale; p++, scale /= 10)
			frac += (*p - '0') * scale * 10;
		ms += frac;
	}
	if (*p || p == argv[1]) {
		printf("sleep: bad interval '%s'\n", argv[1]);
		return 1;
	}
	sleep_ms(ms);
	return 0;
}
