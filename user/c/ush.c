/*
 * ush: a small shell that runs in user mode. Line editing comes from the getkey syscall;
 * programs run through spawn(); '!cmd' hands a line to the kernel shell (ls, cat, ps ...).
 */
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "usys.h"

#define LINE_MAX 100
#define ARGV_MAX 12

static int last_status;

static int readline(char *buf, int max)
{
	int len = 0;

	for (;;) {
		int k = getkey();

		if (k == '\n') {
			putchar('\n');
			buf[len] = 0;
			return len;
		}
		if (k == '\b') {
			if (len) {
				len--;
				putchar('\b');
			}
		} else if (k >= 32 && k < 127 && len < max - 1) {
			buf[len++] = (char)k;
			putchar(k);
		}
	}
}

static int split(char *line, char **argv)
{
	int argc = 0;

	for (;;) {
		while (*line == ' ')
			line++;
		if (!*line || argc == ARGV_MAX - 1)
			break;
		argv[argc++] = line;
		while (*line && *line != ' ')
			line++;
		if (*line)
			*line++ = 0;
	}
	argv[argc] = 0;
	return argc;
}

static void help(void)
{
	puts("ush builtins: help  echo ARGS  status  pid  exit [N]");
	puts("  !CMD   run a kernel shell command (e.g. !ls, !ps)");
	puts("  NAME ARGS   run a program (e.g. hello, primes 50, args a b)");
}

int main(void)
{
	char line[LINE_MAX], *argv[ARGV_MAX];
	int argc;

	puts("ush - user-mode shell. Type 'help'.");
	for (;;) {
		write(1, "ush$ ", 5);
		readline(line, sizeof line);
		if (line[0] == '!') {
			last_status = kcmd(line + 1);
			continue;
		}
		argc = split(line, argv);
		if (!argc)
			continue;
		if (!strcmp(argv[0], "exit"))
			return argc > 1 ? atoi(argv[1]) : 0;
		if (!strcmp(argv[0], "help")) {
			help();
		} else if (!strcmp(argv[0], "echo")) {
			int i;

			for (i = 1; i < argc; i++)
				printf("%s%c", argv[i], i + 1 < argc ? ' ' : '\n');
			if (argc == 1)
				putchar('\n');
			last_status = 0;
		} else if (!strcmp(argv[0], "status")) {
			printf("%d\n", last_status);
		} else if (!strcmp(argv[0], "pid")) {
			printf("%d\n", getpid());
		} else {
			last_status = spawn(argv[0], argv);
			if (last_status == -1)
				printf("ush: %s: not found\n", argv[0]);
			else if (last_status)
				printf("[exit %d]\n", last_status);
		}
	}
}
