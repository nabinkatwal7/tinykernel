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
	int in_func;    /* running a function body: 'return' is allowed */
	int returning;  /* 'return' was run */
};

/* Shell functions, defined by scripts and callable from any command line afterwards. */
#define MAX_FUNCS     16
#define MAX_CALL_DEPTH 16

static struct func {
	char name[24];
	char *text;     /* the body, one command per NUL-terminated line */
	int nlines;
	int used;
} funcs[MAX_FUNCS];

static int call_depth;
static int exit_requested; /* 'exit' inside a function must also end the calling script */
static int run_depth;      /* nested script_run calls */

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
static int syntax(struct ctx *c, const char *what, int line);

/* "name() {" or "function name {": the function name (in a static buffer), or NULL. */
static char *function_header(char *line)
{
	static char name[24];
	char *p = line;
	size_t n = 0;

	if (!kstrncmp(p, "function ", 9))
		p = skip_blanks(p + 9);
	while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' || (n && *p >= '0' && *p <= '9')) {
		if (n < sizeof name - 1)
			name[n++] = *p;
		p++;
	}
	if (!n)
		return 0;
	name[n] = '\0';
	p = skip_blanks(p);
	if (p[0] == '(' && p[1] == ')')
		p = skip_blanks(p + 2);
	else if (kstrncmp(line, "function ", 9))
		return 0;
	return p[0] == '{' && !p[1] ? name : 0;
}

/* Stores lines i+1 .. the next "}" as the body of function 'name'; returns the index of the "}". */
static int define_function(struct ctx *c, int i, int to, const char *name)
{
	int j, k, bytes = 0, slot = -1;
	struct func *f;
	char *p;

	for (j = i + 1; j < to && kstrcmp(c->lines[j], "}"); j++)
		bytes += (int)kstrlen(c->lines[j]) + 1;
	if (j >= to)
		return syntax(c, "function without a closing '}'", i);
	for (k = 0; k < MAX_FUNCS; k++) {
		if (funcs[k].used && !kstrcmp(funcs[k].name, name))
			slot = k;
		else if (!funcs[k].used && slot < 0)
			slot = k;
	}
	if (slot < 0) {
		console_write("sh: too many functions\n");
		return j;
	}
	f = &funcs[slot];
	if (f->used)
		kfree(f->text);
	f->text = kmalloc((size_t)bytes + 1);
	if (!f->text) {
		f->used = 0;
		return j;
	}
	f->used = 1;
	f->nlines = j - i - 1;
	kstrlcpy(f->name, name, sizeof f->name);
	for (p = f->text, k = i + 1; k < j; k++) {
		size_t len = kstrlen(c->lines[k]) + 1;

		memcpy(p, c->lines[k], len);
		p += len;
	}
	return j;
}

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
	while (!c->exiting && !c->returning && !exit_requested && !shell_interrupted() && shell_exec(cond) == 0) {
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

	for (i = from; i < to && !c->exiting && !c->returning && !exit_requested && !c->loop_ctl
	     && !shell_interrupted(); i++) {
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
			exit_requested = 1;
		} else if (!kstrcmp(line, "return") || !kstrncmp(line, "return ", 7)) {
			uint32_t code = (uint32_t)c->status;

			if (!c->in_func) {
				syntax(c, "'return' outside a function", i);
				break;
			}
			if (line[6])
				kstrtoul(skip_blanks(line + 6), &code);
			c->status = (int)code;
			c->returning = 1;
		} else if ((rest = function_header(line))) {
			i = define_function(c, i, to, rest);
		} else {
			c->status = shell_exec(line);
		}
		if (i < 0)
			break;
	}
	return c->status;
}

/* Run function 'name' with the given arguments (argv[0] is the name). 1 if there is such a function. */
int script_call(const char *name, int argc, char **argv, int *status)
{
	char **saved_argv = cur_argv;
	int saved_argc = cur_argc, k;
	struct func *f = 0;
	struct ctx *c;
	char *p;
	int i;

	for (k = 0; k < MAX_FUNCS; k++)
		if (funcs[k].used && !kstrcmp(funcs[k].name, name))
			f = &funcs[k];
	if (!f)
		return 0;
	if (call_depth >= MAX_CALL_DEPTH) {
		console_printf("sh: %s: call nesting too deep\n", name);
		*status = 1;
		return 1;
	}
	c = kcalloc(1, sizeof *c);
	if (!c) {
		*status = 1;
		return 1;
	}
	for (p = f->text, i = 0; i < f->nlines && i < SCRIPT_MAX_LINES; i++) {
		c->lines[c->n++] = p;
		p += kstrlen(p) + 1;
	}
	c->in_func = 1;
	cur_argv = argv;
	cur_argc = argc;
	call_depth++;
	run_range(c, 0, c->n);
	call_depth--;
	*status = c->status;
	cur_argv = saved_argv;
	cur_argc = saved_argc;
	kfree(c);
	return 1;
}

int script_function_count(void)
{
	int k, n = 0;

	for (k = 0; k < MAX_FUNCS; k++)
		n += funcs[k].used;
	return n;
}

const char *script_function_name(int index)
{
	int k;

	for (k = 0; k < MAX_FUNCS; k++)
		if (funcs[k].used && index-- == 0)
			return funcs[k].name;
	return 0;
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
	run_depth++;
	run_range(c, 0, c->n);
	if (!--run_depth)
		exit_requested = 0;
	status = c->status;
	cur_argv = saved_argv;
	cur_argc = saved_argc;
	kfree(text);
	kfree(c);
	return status;
}
