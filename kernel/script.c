#include "script.h"

#include "console.h"
#include "fs.h"
#include "kmalloc.h"
#include "kstring.h"
#include "shell.h"
#include "vfs.h"

static char **cur_argv;
static int cur_argc;

const char *script_param(int n)
{
	if (!cur_argv || n < 0 || n >= cur_argc)
		return 0;
	return cur_argv[n];
}

int script_nargs(void)
{
	return cur_argc > 0 ? cur_argc - 1 : 0;
}

static char *skip_blanks(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

int script_run(const char *path, int argc, char **argv)
{
	static const char *const none[1] = { 0 };
	char **saved_argv = cur_argv;
	int saved_argc = cur_argc;
	char *text, *lines[SCRIPT_MAX_LINES];
	int size, n = 0, i, status = 0;

	size = vfs_size(path);
	if (size < 0) {
		console_printf("sh: cannot read %s: %s\n", path, fs_strerror(size));
		return 127;
	}
	if (size > SCRIPT_MAX_SIZE) {
		console_printf("sh: %s is too large\n", path);
		return 1;
	}
	text = kmalloc((size_t)size + 1);
	if (!text)
		return 1;
	size = size ? vfs_read(path, text, (uint32_t)size) : 0;
	if (size < 0) {
		kfree(text);
		console_printf("sh: cannot read %s\n", path);
		return 127;
	}
	text[size] = '\0';

	for (char *p = text; *p && n < SCRIPT_MAX_LINES;) {
		lines[n++] = p;
		while (*p && *p != '\n')
			p++;
		if (*p)
			*p++ = '\0';
	}

	cur_argv = argc > 0 ? argv : (char **)none;
	cur_argc = argc;
	for (i = 0; i < n && !shell_interrupted(); i++) {
		char *line = skip_blanks(lines[i]);
		size_t len = kstrlen(line);

		while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
			line[--len] = '\0';
		if (!*line || *line == '#')
			continue;
		if (!kstrcmp(line, "exit") || !kstrncmp(line, "exit ", 5)) {
			uint32_t code = 0;

			if (line[4])
				kstrtoul(skip_blanks(line + 4), &code);
			status = (int)code;
			break;
		}
		status = shell_exec(line);
	}
	cur_argv = saved_argv;
	cur_argc = saved_argc;
	kfree(text);
	return status;
}
