/* Self-test for the user-space string functions. Exit code = number of failed checks. */
#include "string.h"
#include "usys.h"

static int fails;

static void check(int ok, const char *what)
{
	if (!ok) {
		write("FAIL: ", 6);
		write(what, (int)strlen(what));
		putchar('\n');
		fails++;
	}
}

int main(void)
{
	char buf[32], big[64];

	check(strlen("") == 0 && strlen("hello") == 5, "strlen");
	check(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0, "strcmp");
	check(strncmp("abcdef", "abcxyz", 3) == 0 && strncmp("abcdef", "abcxyz", 4) < 0, "strncmp");
	strcpy(buf, "tiny");
	check(!strcmp(buf, "tiny"), "strcpy");
	strcat(buf, "os");
	check(!strcmp(buf, "tinyos"), "strcat");
	memset(big, 'x', sizeof big);
	strncpy(big, "ab", 5);
	check(big[0] == 'a' && big[1] == 'b' && big[2] == 0 && big[4] == 0 && big[5] == 'x', "strncpy pads");
	check(*strchr("a/b/c", '/') == '/' && strchr("abc", 'z') == 0, "strchr");
	check(strrchr("a/b/c", '/') && strrchr("a/b/c", '/')[1] == 'c', "strrchr");
	check(strstr("hello world", "o w") && strstr("hello", "xyz") == 0 && !strcmp(strstr("abc", ""), "abc"),
	      "strstr");
	strcpy(buf, "abcdef");
	memmove(buf + 2, buf, 4);
	check(!strcmp(buf, "ababcd"), "memmove overlap forward");
	strcpy(buf, "abcdef");
	memmove(buf, buf + 2, 4);
	check(!strcmp(buf, "cdefef"), "memmove overlap backward");
	check(memcmp("abc", "abd", 3) < 0 && memcmp("abc", "abc", 3) == 0, "memcmp");
	check(atoi("42") == 42 && atoi("-17") == -17 && atoi("  +8x") == 8, "atoi");

	if (fails == 0)
		write("strtest: all checks passed\n", 27);
	return fails;
}
