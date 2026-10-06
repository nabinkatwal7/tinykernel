#include "textutil.h"

#include <stdint.h>

#include "console.h"
#include "file.h"
#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "pcache.h"
#include "shell.h"
#include "vfs.h"

#define STDIN_MAX (64 * 1024)

/* The text of one input: a NUL-terminated buffer owned by the caller (kfree). */
struct text {
	char *data;
	uint32_t len;
};

/* Reads a file, or standard input when path is NULL. Returns 0, or 1 after printing why not. */
static int load_text(const char *path, struct text *t)
{
	char *buf;
	int n;

	t->data = 0;
	t->len = 0;
	if (path) {
		n = vfs_size(path);
		if (n < 0) {
			console_printf("%s: %s\n", path, fs_strerror(n));
			return 1;
		}
		buf = kmalloc((size_t)n + 1);
		if (!buf) {
			console_write("out of memory\n");
			return 1;
		}
		n = n ? pcache_read(path, buf, (uint32_t)n) : 0;
		if (n < 0) {
			kfree(buf);
			console_printf("%s: %s\n", path, fs_strerror(n));
			return 1;
		}
		buf[n] = '\0';
		t->data = buf;
		t->len = (uint32_t)n;
		return 0;
	}
	if (!file_stdin_active()) {
		console_write("no input: give a file name or use a pipe\n");
		return 1;
	}
	buf = kmalloc(STDIN_MAX + 1);
	if (!buf) {
		console_write("out of memory\n");
		return 1;
	}
	while (t->len < STDIN_MAX && (n = console_stdin_read(buf + t->len, STDIN_MAX - t->len)) > 0)
		t->len += (uint32_t)n;
	buf[t->len] = '\0';
	t->data = buf;
	return 0;
}

static char lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int equal_at(const char *s, const char *pat, size_t n, int icase)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (icase ? lower(s[i]) != lower(pat[i]) : s[i] != pat[i])
			return 0;
	}
	return 1;
}

/* Does the line contain pat? A leading '^' anchors it to the start, a trailing '$' to the end. */
static int line_matches(const char *line, const char *pat, int icase)
{
	size_t plen = kstrlen(pat), llen = kstrlen(line), i;
	int head = 0, tail = 0;

	if (plen && pat[0] == '^') {
		head = 1;
		pat++;
		plen--;
	}
	if (plen && pat[plen - 1] == '$') {
		tail = 1;
		plen--;
	}
	if (plen > llen)
		return 0;
	if (head && tail)
		return plen == llen && equal_at(line, pat, plen, icase);
	if (head)
		return equal_at(line, pat, plen, icase);
	if (tail)
		return equal_at(line + llen - plen, pat, plen, icase);
	for (i = 0; i + plen <= llen; i++)
		if (equal_at(line + i, pat, plen, icase))
			return 1;
	return 0;
}

int tu_grep(int argc, char **argv)
{
	int invert = 0, icase = 0, number = 0, count = 0, a = 1, matched = 0, rc = 1, nfiles, f;
	const char *pat;

	while (a < argc && argv[a][0] == '-' && argv[a][1]) {
		const char *o;

		for (o = argv[a] + 1; *o; o++) {
			switch (*o) {
			case 'v': invert = 1; break;
			case 'i': icase = 1; break;
			case 'n': number = 1; break;
			case 'c': count = 1; break;
			default:
				console_printf("grep: unknown option -%c\n", *o);
				return 2;
			}
		}
		a++;
	}
	if (a >= argc) {
		console_write("usage: grep [-ivnc] pattern [file...]\n");
		return 2;
	}
	pat = argv[a++];
	nfiles = argc - a;
	for (f = 0; f < (nfiles ? nfiles : 1); f++) {
		const char *name = nfiles ? argv[a + f] : 0;
		struct text t;
		char *p, *eol;
		int lineno = 0, hits = 0;

		if (load_text(name, &t)) {
			rc = 2;
			continue;
		}
		for (p = t.data; p < t.data + t.len; p = eol + 1) {
			int hit;

			eol = p;
			while (eol < t.data + t.len && *eol != '\n')
				eol++;
			*eol = '\0'; /* the buffer has a spare byte after its end */
			lineno++;
			hit = line_matches(p, pat, icase) != invert;
			if (hit) {
				hits++;
				matched = 1;
				if (!count) {
					if (nfiles > 1)
						console_printf("%s:", name);
					if (number)
						console_printf("%d:", lineno);
					console_printf("%s\n", p);
				}
			}
		}
		if (count) {
			if (nfiles > 1)
				console_printf("%s:", name);
			console_printf("%d\n", hits);
		}
		kfree(t.data);
		if (shell_interrupted())
			break;
	}
	return rc == 2 ? 2 : !matched;
}

int tu_wc(int argc, char **argv)
{
	int a = 1, want_l = 0, want_w = 0, want_c = 0, nfiles, f, rc = 0;
	uint32_t tl = 0, tw = 0, tc = 0;

	while (a < argc && argv[a][0] == '-' && argv[a][1]) {
		const char *o;

		for (o = argv[a] + 1; *o; o++) {
			switch (*o) {
			case 'l': want_l = 1; break;
			case 'w': want_w = 1; break;
			case 'c': want_c = 1; break;
			default:
				console_printf("wc: unknown option -%c\n", *o);
				return 2;
			}
		}
		a++;
	}
	if (!want_l && !want_w && !want_c)
		want_l = want_w = want_c = 1;
	nfiles = argc - a;
	for (f = 0; f < (nfiles ? nfiles : 1); f++) {
		const char *name = nfiles ? argv[a + f] : 0;
		struct text t;
		uint32_t i, lines = 0, words = 0;
		int in_word = 0;

		if (load_text(name, &t)) {
			rc = 1;
			continue;
		}
		for (i = 0; i < t.len; i++) {
			char c = t.data[i];

			if (c == '\n')
				lines++;
			if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
				in_word = 0;
			} else if (!in_word) {
				in_word = 1;
				words++;
			}
		}
		if (want_l)
			console_printf("%7u", lines);
		if (want_w)
			console_printf("%7u", words);
		if (want_c)
			console_printf("%7u", t.len);
		if (name)
			console_printf(" %s", name);
		console_putchar('\n');
		tl += lines;
		tw += words;
		tc += t.len;
		kfree(t.data);
	}
	if (nfiles > 1) {
		if (want_l)
			console_printf("%7u", tl);
		if (want_w)
			console_printf("%7u", tw);
		if (want_c)
			console_printf("%7u", tc);
		console_write(" total\n");
	}
	return rc;
}

/* Parses "-n N" / "-N" at argv[*a]; leaves the line count in *n. 0 on success. */
static int parse_count(int argc, char **argv, int *a, uint32_t *n, const char *cmd)
{
	*n = 10;
	while (*a < argc && argv[*a][0] == '-' && argv[*a][1]) {
		const char *num = argv[*a] + 1;

		if (!kstrcmp(argv[*a], "-n")) {
			if (*a + 1 >= argc)
				return 1;
			num = argv[++*a];
		}
		if (kstrtoul(num, n)) {
			console_printf("%s: bad line count '%s'\n", cmd, num);
			return 1;
		}
		(*a)++;
	}
	return 0;
}

static void write_text(const char *s, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++)
		console_putchar(s[i]);
}

/* head -n N / tail -n N over each input; 'last' selects tail. */
static int head_or_tail(int argc, char **argv, int last)
{
	int a = 1, nfiles, f, rc = 0;
	uint32_t n;

	if (parse_count(argc, argv, &a, &n, last ? "tail" : "head"))
		return 2;
	nfiles = argc - a;
	for (f = 0; f < (nfiles ? nfiles : 1); f++) {
		const char *name = nfiles ? argv[a + f] : 0;
		struct text t;
		uint32_t i, seen = 0;

		if (load_text(name, &t)) {
			rc = 1;
			continue;
		}
		if (nfiles > 1)
			console_printf("%s==> %s <==\n", f ? "\n" : "", name);
		if (!last) {
			for (i = 0; i < t.len && seen < n; i++) {
				console_putchar(t.data[i]);
				if (t.data[i] == '\n')
					seen++;
			}
		} else {
			uint32_t end = t.len, start = end;

			if (end && t.data[end - 1] == '\n')
				end--; /* the final newline ends the last line, it does not start another */
			for (start = end; start > 0; start--) {
				if (t.data[start - 1] == '\n' && ++seen >= n)
					break;
			}
			if (!n)
				start = t.len;
			write_text(t.data + start, t.len - start);
		}
		kfree(t.data);
	}
	return rc;
}

int tu_head(int argc, char **argv)
{
	return head_or_tail(argc, argv, 0);
}

int tu_tail(int argc, char **argv)
{
	return head_or_tail(argc, argv, 1);
}

#define SORT_MAX_LINES 2048
#define SORT_MAX_INPUTS 8

static int sort_numeric, sort_reverse;

static int32_t leading_number(const char *s)
{
	int32_t v = 0;
	int neg = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '-') {
		neg = 1;
		s++;
	}
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return neg ? -v : v;
}

static int line_compare(const char *a, const char *b)
{
	int c;

	if (sort_numeric) {
		int32_t x = leading_number(a), y = leading_number(b);

		c = x < y ? -1 : x > y;
		if (!c)
			c = kstrcmp(a, b);
	} else {
		c = kstrcmp(a, b);
	}
	return sort_reverse ? -c : c;
}

/* Stable merge sort of the pointer array. */
static void sort_lines(char **v, char **tmp, int n)
{
	int mid, i, j, k;

	if (n < 2)
		return;
	mid = n / 2;
	sort_lines(v, tmp, mid);
	sort_lines(v + mid, tmp, n - mid);
	for (i = 0, j = mid, k = 0; i < mid || j < n;)
		tmp[k++] = (j >= n || (i < mid && line_compare(v[i], v[j]) <= 0)) ? v[i++] : v[j++];
	for (i = 0; i < n; i++)
		v[i] = tmp[i];
}

int tu_sort(int argc, char **argv)
{
	static char *lines[SORT_MAX_LINES], *tmp[SORT_MAX_LINES];
	struct text texts[SORT_MAX_INPUTS];
	int a = 1, unique = 0, nfiles, f, n = 0, ntexts = 0, i, rc = 0;

	sort_numeric = sort_reverse = 0;
	while (a < argc && argv[a][0] == '-' && argv[a][1]) {
		const char *o;

		for (o = argv[a] + 1; *o; o++) {
			switch (*o) {
			case 'n': sort_numeric = 1; break;
			case 'r': sort_reverse = 1; break;
			case 'u': unique = 1; break;
			default:
				console_printf("sort: unknown option -%c\n", *o);
				return 2;
			}
		}
		a++;
	}
	nfiles = argc - a;
	if (nfiles > SORT_MAX_INPUTS) {
		console_write("sort: too many files\n");
		return 2;
	}
	for (f = 0; f < (nfiles ? nfiles : 1); f++) {
		struct text *t = &texts[ntexts];
		char *p, *eol;

		if (load_text(nfiles ? argv[a + f] : 0, t)) {
			rc = 2;
			continue;
		}
		ntexts++;
		for (p = t->data; p < t->data + t->len; p = eol + 1) {
			eol = p;
			while (eol < t->data + t->len && *eol != '\n')
				eol++;
			*eol = '\0';
			if (n < SORT_MAX_LINES) {
				lines[n++] = p;
			} else {
				console_write("sort: too many lines, the rest is ignored\n");
				break;
			}
		}
	}
	sort_lines(lines, tmp, n);
	for (i = 0; i < n; i++) {
		if (unique && i && !kstrcmp(lines[i], lines[i - 1]))
			continue;
		console_printf("%s\n", lines[i]);
	}
	for (i = 0; i < ntexts; i++)
		kfree(texts[i].data);
	return rc;
}

/* Shell-style wildcard match: '*' any run, '?' any one character. */
static int glob_match(const char *pat, const char *s)
{
	if (!*pat)
		return !*s;
	if (*pat == '*') {
		do {
			if (glob_match(pat + 1, s))
				return 1;
		} while (*s++);
		return 0;
	}
	if (*s && (*pat == '?' || *pat == *s))
		return glob_match(pat + 1, s + 1);
	return 0;
}

#define FIND_MAX_DEPTH 8

struct find_opts {
	const char *name; /* -name pattern, or NULL */
	char type;        /* 'f', 'd' or 0 */
	int hits;
};

static void find_walk(const char *dir, int depth, struct find_opts *o)
{
	struct vfs_dirent *ent = kmalloc(sizeof *ent * FS_MAX_FILES);
	int n, i;

	if (!ent)
		return;
	n = vfs_list(dir, ent, FS_MAX_FILES);
	for (i = 0; i < n && !shell_interrupted(); i++) {
		char path[VFS_PATH_MAX];

		if (!kstrcmp(dir, "/"))
			ksnprintf(path, sizeof path, "/%s", ent[i].name);
		else if (!kstrcmp(dir, "."))
			ksnprintf(path, sizeof path, "%s", ent[i].name);
		else
			ksnprintf(path, sizeof path, "%s/%s", dir, ent[i].name);
		if ((!o->name || glob_match(o->name, ent[i].name)) && (!o->type || (o->type == 'd') == (ent[i].is_dir != 0))) {
			console_printf("%s\n", path);
			o->hits++;
		}
		if (ent[i].is_dir && depth < FIND_MAX_DEPTH)
			find_walk(path, depth + 1, o);
	}
	kfree(ent);
}

int tu_find(int argc, char **argv)
{
	struct find_opts o = { 0, 0, 0 };
	const char *start = ".";
	int a = 1;

	if (a < argc && argv[a][0] != '-')
		start = argv[a++];
	while (a < argc) {
		if (!kstrcmp(argv[a], "-name") && a + 1 < argc) {
			o.name = argv[a + 1];
		} else if (!kstrcmp(argv[a], "-type") && a + 1 < argc && (argv[a + 1][0] == 'f' || argv[a + 1][0] == 'd')) {
			o.type = argv[a + 1][0];
		} else {
			console_write("usage: find [dir] [-name pattern] [-type f|d]\n");
			return 2;
		}
		a += 2;
	}
	find_walk(start, 0, &o);
	return !o.hits;
}

#define DIFF_MAX_LINES 400

/* Splits a text into lines in place; returns how many (at most max). */
static int split_lines(struct text *t, char **v, int max)
{
	char *p = t->data, *eol;
	int n = 0;

	while (p < t->data + t->len && n < max) {
		eol = p;
		while (eol < t->data + t->len && *eol != '\n')
			eol++;
		*eol = '\0';
		v[n++] = p;
		p = eol + 1;
	}
	return n;
}

static void range(int from, int to) /* 1-based, inclusive; prints "a" or "a,b" */
{
	if (from == to)
		console_printf("%d", from);
	else
		console_printf("%d,%d", from, to);
}

/* diff file1 file2 : the lines that differ, in the classic "ed" style (NcM / NaM / NdM, '<' old, '>' new) */
int tu_diff(int argc, char **argv)
{
	static char *va[DIFF_MAX_LINES], *vb[DIFF_MAX_LINES];
	struct text ta, tb;
	uint16_t *lcs;
	int na, nb, i = 0, j = 0, differ = 0, w, del, add;

	if (argc != 3) {
		console_write("usage: diff file1 file2\n");
		return 2;
	}
	if (load_text(argv[1], &ta))
		return 2;
	if (load_text(argv[2], &tb)) {
		kfree(ta.data);
		return 2;
	}
	na = split_lines(&ta, va, DIFF_MAX_LINES);
	nb = split_lines(&tb, vb, DIFF_MAX_LINES);
	if ((na == DIFF_MAX_LINES && ta.len) || (nb == DIFF_MAX_LINES && tb.len))
		console_printf("diff: only the first %d lines of each file are compared\n", DIFF_MAX_LINES);
	w = nb + 1;
	lcs = kcalloc((size_t)(na + 1) * (size_t)w, sizeof *lcs);
	if (!lcs) {
		console_write("diff: out of memory\n");
		kfree(ta.data);
		kfree(tb.data);
		return 2;
	}
	/* lcs[i][j]: length of the longest common subsequence of a[i..] and b[j..] */
	for (i = na - 1; i >= 0; i--)
		for (j = nb - 1; j >= 0; j--)
			lcs[i * w + j] = !kstrcmp(va[i], vb[j]) ? (uint16_t)(lcs[(i + 1) * w + j + 1] + 1)
				: (lcs[(i + 1) * w + j] >= lcs[i * w + j + 1] ? lcs[(i + 1) * w + j] : lcs[i * w + j + 1]);
	i = j = 0;
	while (i < na || j < nb) {
		int si = i, sj = j;

		if (i < na && j < nb && !kstrcmp(va[i], vb[j])) {
			i++;
			j++;
			continue;
		}
		/* a run of differences: advance until the files line up again */
		while ((i < na || j < nb) && !(i < na && j < nb && !kstrcmp(va[i], vb[j]))) {
			if (j >= nb || (i < na && lcs[(i + 1) * w + j] >= lcs[i * w + j + 1]))
				i++;
			else
				j++;
		}
		differ = 1;
		del = i - si;
		add = j - sj;
		if (del)
			range(si + 1, i);
		else
			console_printf("%d", si);
		console_putchar(del && add ? 'c' : del ? 'd' : 'a');
		if (add)
			range(sj + 1, j);
		else
			console_printf("%d", sj);
		console_putchar('\n');
		for (; si < i; si++)
			console_printf("< %s\n", va[si]);
		if (del && add)
			console_write("---\n");
		for (; sj < j; sj++)
			console_printf("> %s\n", vb[sj]);
	}
	kfree(lcs);
	kfree(ta.data);
	kfree(tb.data);
	return differ;
}
