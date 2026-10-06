/* Checks the user-space printf family against known strings, then shows live printf output. */
#include "stdio.h"
#include "string.h"

static int fails;

static void expect(const char *got, const char *want)
{
	if (strcmp(got, want)) {
		printf("FAIL: got '%s' want '%s'\n", got, want);
		fails++;
	}
}

int main(int argc, char **argv)
{
	char b[64];

	snprintf(b, sizeof b, "%d|%u|%x|%X", -42, 42u, 255u, 255u);
	expect(b, "-42|42|ff|FF");
	snprintf(b, sizeof b, "[%5d][%-5d][%05d]", 42, 42, 42);
	expect(b, "[   42][42   ][00042]");
	snprintf(b, sizeof b, "[%s][%8s][%-8s][%c]", "hi", "right", "left", 'z');
	expect(b, "[hi][   right][left    ][z]");
	snprintf(b, sizeof b, "%d %d", -2147483647 - 1, 2147483647);
	expect(b, "-2147483648 2147483647");
	snprintf(b, sizeof b, "100%% %s", (char *)0);
	expect(b, "100% (null)");
	snprintf(b, sizeof b, "%p", (void *)0x1234);
	expect(b, "0x00001234");
	snprintf(b, 6, "abcdefghij");
	expect(b, "abcde");
	if (snprintf(b, sizeof b, "%s", "hello") != 5) {
		printf("FAIL: snprintf return value\n");
		fails++;
	}

	printf("printf live: argc=%d argv[0]=%s %08x\n", argc, argv[0], 0xBEEF);
	puts("puts adds a newline");
	if (!fails)
		puts("printftest: all checks passed");
	return fails;
}
