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

/* One running script: its lines, and the control state shared by nested blocks. */
struct ctx {
	char *lines[SCRIPT_MAX_LINES];
	int n;
	int status;     /* status of the last command */
	int exiting;    /* 'exit' was run: unwind everything */
	int loop_ctl;   /* CTL_BREAK / CTL_CONTINUE requested by the innermost loop body */
	int depth;      /* nesting of loops, so that 'break' outside one is an error */
};

enum { CTL_NONE, CTL_BREAK, CTL_CONTINUE };

static char *skip_blanks(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

/* Does the line begin with the word kw (followed by a space or the end)? Returns the text after it, or NULL. */
static char *keyword(char *line, const char *kw)
{
	size_t n = kstrlen(kw);

	if (kstrncmp(line, kw, n) || (line[n] && line[n] != ' ' && line[n] != '\t'))
		return 0;
	return skip_blanks(line + n);
}

/*
 * Index of the line that closes the construct opened at 'open' (its "fi" or "done"), or -1. For an "if",
 * *other receives the index of its top-level "else" (or -1).
 */
static int find_close(struct ctx *c, int open, int limit, int *other)
{
	int j, depth = 1;

	if (other)
		*other = -1;
	for (j = open + 1; j < limit; j++) {
		char *l = c->lines[j];

		if (keyword(l, "if") || keyword(l, "while"))
			depth++;
		else if (keyword(l, "fi") || keyword(l, "done"))
			depth--;
		else if (other && depth == 1 && *other < 0 && keyword(l, "else"))
			*other = j;
		if (!depth)
			return j;
	}
	return -1;
}

static int run_range(struct ctx *c, int from, int to);

static int syntax(struct ctx *c, const char *what, int line)
{
	console_printf("sh: line %d: %s\n", line + 1, what);
	c->status = 2;
	c->exiting = 1;
	return -1;
}

/* if COMMAND / then / ... / [else / ...] / fi : returns the index of the "fi", or -1 after a syntax error. */
static int run_if(struct ctx *c, int i, int to, char *cond)
{
	int other, fi = find_close(c, i, to, &other), then_end, ok;

	if (fi < 0)
		return syntax(c, "'if' without 'fi'", i);
	if (i + 1 >= fi || !keyword(c->lines[i + 1], "then"))
		return syntax(c, "expected 'then' after 'if'", i);
	ok = shell_exec(cond) == 0;
	then_end = other >= 0 ? other : fi;
	if (ok)
		run_range(c, i + 2, then_end);
	else if (other >= 0)
		run_range(c, other + 1, fi);
	return fi;
}

/* while COMMAND / do / ... / done */
static int run_while(struct ctx *c, int i, int to, char *cond)
{
	int done = find_close(c, i, to, 0), iterations = 0;

	if (done < 0)
		return syntax(c, "'while' without 'done'", i);
	if (i + 1 >= done || !keyword(c->lines[i + 1], "do"))
		return syntax(c, "expected 'do' after 'while'", i);
	c->depth++;
	while (!c->exiting && !shell_interrupted() && shell_exec(cond) == 0) {
		if (++iterations > SCRIPT_MAX_LOOPS) {
			console_write("sh: loop stopped after too many iterations\n");
			break;
		}
		run_range(c, i + 2, done);
		if (c->loop_ctl == CTL_BREAK) {
			c->loop_ctl = CTL_NONE;
			break;
		}
		c->loop_ctl = CTL_NONE; /* continue: just go round again */
	}
	c->depth--;
	return done;
}

static int run_range(struct ctx *c, int from, int to)
{
	int i;

	for (i = from; i < to && !c->exiting && !c->loop_ctl && !shell_interrupted(); i++) {
		char *line = c->lines[i], *rest;

		if ((rest = keyword(line, "if"))) {
			i = run_if(c, i, to, rest);
		} else if ((rest = keyword(line, "while"))) {
			i = run_while(c, i, to, rest);
		} else if (keyword(line, "then") || keyword(line, "else") || keyword(line, "fi")
			   || keyword(line, "do") || keyword(line, "done")) {
			syntax(c, "unexpected keyword", i);
		} else if (!kstrcmp(line, "break") || !kstrcmp(line, "continue")) {
			if (!c->depth) {
				syntax(c, "'break'/'continue' outside a loop", i);
				break;
			}
			c->loop_ctl = line[0] == 'b' ? CTL_BREAK : CTL_CONTINUE;
		} else if (!kstrcmp(line, "exit") || !kstrncmp(line, "exit ", 5)) {
			uint32_t code = 0;

			if (line[4])
				kstrtoul(skip_blanks(line + 4), &code);
			c->status = (int)code;
			c->exiting = 1;
		} else {
			c->status = shell_exec(line);
		}
		if (i < 0)
			break;
	}
	return c->status;
}

int script_run(const char *path, int argc, char **argv)
{
	static const char *const none[1] = { 0 };
	struct ctx *c;
	char **saved_argv = cur_argv;
	int saved_argc = cur_argc, size, status;
	char *text, *p;

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
	c = kcalloc(1, sizeof *c);
	if (!text || !c) {
		kfree(text);
		kfree(c);
		return 1;
	}
	size = size ? vfs_read(path, text, (uint32_t)size) : 0;
	if (size < 0) {
		kfree(text);
		kfree(c);
		console_printf("sh: cannot read %s\n", path);
		return 127;
	}
	text[size] = '\0';

	/* Split into lines, trimming blanks and dropping comments and empty lines. */
	for (p = text; *p && c->n < SCRIPT_MAX_LINES;) {
		char *line = skip_blanks(p);
		size_t len;

		while (*p && *p != '\n')
			p++;
		if (*p)
			*p++ = '\0';
		len = kstrlen(line);
		while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t'))
			line[--len] = '\0';
		if (*line && *line != '#')
			c->lines[c->n++] = line;
	}

	cur_argv = argc > 0 ? argv : (char **)none;
	cur_argc = argc;
	run_range(c, 0, c->n);
	status = c->status;
	cur_argv = saved_argv;
	cur_argc = saved_argc;
	kfree(text);
	kfree(c);
	return status;
}
