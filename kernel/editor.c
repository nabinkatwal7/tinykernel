#include "editor.h"

#include "console.h"
#include "fs.h"
#include "keyboard.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "vfs.h"

/*
 * A small modeless editor in the style of nano: type to insert, arrows/Home/End to move,
 * Backspace/Delete to erase, Ctrl+S saves, Ctrl+Q quits (twice if there are unsaved changes).
 * The text is an array of fixed-width lines; a line longer than the screen is wrapped when loaded.
 */
#define MAX_LINES 400
#define LINE_W    78        /* characters per line (the screen is 80 wide) */
#define VIEW_ROWS (CONSOLE_LAST_ROW - CONSOLE_FIRST_ROW) /* 23 text rows, last row = status */

#define ATTR_TEXT   0x07
#define ATTR_STATUS 0x70

struct editor {
	char (*line)[LINE_W + 1];  /* MAX_LINES NUL-terminated lines */
	int nlines;
	int cx, cy;                /* cursor: column, line */
	int top;                   /* first visible line */
	int dirty;
	char path[VFS_PATH_MAX];
	char msg[48];              /* transient status message */
};

static void set_msg(struct editor *e, const char *m)
{
	kstrlcpy(e->msg, m, sizeof e->msg);
}

static int add_line(struct editor *e, int at)
{
	if (e->nlines >= MAX_LINES)
		return -1;
	memmove(e->line[at + 1], e->line[at], (size_t)(e->nlines - at) * (LINE_W + 1));
	e->line[at][0] = '\0';
	e->nlines++;
	return 0;
}

static void del_line(struct editor *e, int at)
{
	memmove(e->line[at], e->line[at + 1], (size_t)(e->nlines - at - 1) * (LINE_W + 1));
	e->nlines--;
	if (!e->nlines) {
		e->nlines = 1;
		e->line[0][0] = '\0';
	}
}

/* Load the file (if it exists), splitting on '\n' and wrapping over-long lines. */
static void load(struct editor *e)
{
	int size = vfs_size(e->path);
	char *buf;
	int i, col = 0, ln = 0;

	e->nlines = 1;
	e->line[0][0] = '\0';
	if (size <= 0)
		return;
	buf = kmalloc((size_t)size);
	if (!buf || vfs_read(e->path, buf, (uint32_t)size) < 0) {
		kfree(buf);
		return;
	}
	for (i = 0; i < size; i++) {
		char c = buf[i];

		if (c == '\r')
			continue;
		if (c == '\n' || col >= LINE_W) {
			if (add_line(e, ln + 1))
				break;
			ln++;
			col = 0;
			if (c == '\n')
				continue;
		}
		if (c == '\t')
			c = ' ';
		e->line[ln][col++] = c;
		e->line[ln][col] = '\0';
	}
	/* a file ending in '\n' produced a trailing empty line: drop it */
	if (e->nlines > 1 && !e->line[e->nlines - 1][0] && size && buf[size - 1] == '\n')
		e->nlines--;
	kfree(buf);
}

static int save(struct editor *e)
{
	uint32_t total = 0, off = 0;
	char *buf;
	int i, rc;

	for (i = 0; i < e->nlines; i++)
		total += (uint32_t)kstrlen(e->line[i]) + 1;
	buf = kmalloc(total ? total : 1);
	if (!buf)
		return FS_ENOSPC;
	for (i = 0; i < e->nlines; i++) {
		uint32_t n = (uint32_t)kstrlen(e->line[i]);

		memcpy(buf + off, e->line[i], n);
		off += n;
		buf[off++] = '\n';
	}
	rc = vfs_write(e->path, buf, off);
	kfree(buf);
	return rc;
}

static void draw(struct editor *e)
{
	char status[CONSOLE_COLS + 1];
	int row, x, len;

	for (row = 0; row < VIEW_ROWS; row++) {
		int ln = e->top + row;
		const char *s = ln < e->nlines ? e->line[ln] : 0;

		for (x = 0; x < CONSOLE_COLS; x++)
			console_putat(x, CONSOLE_FIRST_ROW + row, s && s[x] && x < LINE_W ? s[x] : ' ',
				      ATTR_TEXT);
		if (!s) /* past the end of the file: mark with a tilde like vi */
			console_putat(0, CONSOLE_FIRST_ROW + row, '~', 0x08);
	}
	len = ksnprintf(status, sizeof status, " %s%s  Ln %d, Col %d   %s", e->path,
			e->dirty ? " [modified]" : "", e->cy + 1, e->cx + 1,
			e->msg[0] ? e->msg : "^S save  ^Q quit");
	for (x = 0; x < CONSOLE_COLS; x++)
		console_putat(x, CONSOLE_LAST_ROW, x < len ? status[x] : ' ', ATTR_STATUS);
	console_set_hw_cursor(e->cx, CONSOLE_FIRST_ROW + (e->cy - e->top));
}

static void clamp(struct editor *e)
{
	int len;

	if (e->cy < 0)
		e->cy = 0;
	if (e->cy >= e->nlines)
		e->cy = e->nlines - 1;
	len = (int)kstrlen(e->line[e->cy]);
	if (e->cx > len)
		e->cx = len;
	if (e->cx < 0)
		e->cx = 0;
	if (e->cy < e->top)
		e->top = e->cy;
	if (e->cy >= e->top + VIEW_ROWS)
		e->top = e->cy - VIEW_ROWS + 1;
}

static void insert_char(struct editor *e, char c)
{
	char *l = e->line[e->cy];
	int len = (int)kstrlen(l);

	if (len >= LINE_W) {
		set_msg(e, "line is full");
		return;
	}
	memmove(l + e->cx + 1, l + e->cx, (size_t)(len - e->cx + 1));
	l[e->cx++] = c;
	e->dirty = 1;
}

static void newline(struct editor *e)
{
	char *l = e->line[e->cy];

	if (add_line(e, e->cy + 1)) {
		set_msg(e, "too many lines");
		return;
	}
	kstrlcpy(e->line[e->cy + 1], l + e->cx, LINE_W + 1);
	l[e->cx] = '\0';
	e->cy++;
	e->cx = 0;
	e->dirty = 1;
}

static void backspace(struct editor *e)
{
	char *l = e->line[e->cy];

	if (e->cx > 0) {
		memmove(l + e->cx - 1, l + e->cx, kstrlen(l + e->cx) + 1);
		e->cx--;
		e->dirty = 1;
	} else if (e->cy > 0) { /* join with the previous line */
		char *p = e->line[e->cy - 1];
		int plen = (int)kstrlen(p);

		if (plen + (int)kstrlen(l) > LINE_W) {
			set_msg(e, "lines too long to join");
			return;
		}
		kstrlcpy(p + plen, l, LINE_W + 1 - (size_t)plen);
		del_line(e, e->cy);
		e->cy--;
		e->cx = plen;
		e->dirty = 1;
	}
}

static void delete_char(struct editor *e)
{
	char *l = e->line[e->cy];

	if (l[e->cx]) {
		memmove(l + e->cx, l + e->cx + 1, kstrlen(l + e->cx));
		e->dirty = 1;
	} else if (e->cy + 1 < e->nlines) { /* at end of line: pull the next line up */
		e->cy++;
		e->cx = 0;
		backspace(e);
	}
}

int editor_run(const char *path)
{
	struct editor e;
	int k, quit_armed = 0;

	memset(&e, 0, sizeof e);
	e.line = kmalloc(MAX_LINES * (LINE_W + 1));
	if (!e.line) {
		console_write("edit: out of memory\n");
		return 1;
	}
	if (vfs_normalize(vfs_getcwd(), path, e.path, sizeof e.path)) {
		kfree(e.line);
		console_write("edit: bad path\n");
		return 1;
	}
	load(&e);
	if (vfs_size(e.path) < 0)
		set_msg(&e, "new file");

	for (;;) {
		clamp(&e);
		draw(&e);
		k = keyboard_getkey();
		e.msg[0] = '\0';

		if (k == 0x11) { /* Ctrl+Q */
			if (e.dirty && !quit_armed) {
				set_msg(&e, "unsaved changes! ^Q again to discard");
				quit_armed = 1;
				continue;
			}
			break;
		}
		quit_armed = 0;
		switch (k) {
		case 0x13: { /* Ctrl+S */
			int rc = save(&e);

			if (rc) {
				ksnprintf(e.msg, sizeof e.msg, "save failed: %s", fs_strerror(rc));
			} else {
				e.dirty = 0;
				set_msg(&e, "saved");
			}
			break;
		}
		case KEY_UP:    e.cy--; break;
		case KEY_DOWN:  e.cy++; break;
		case KEY_LEFT:
			if (e.cx > 0) {
				e.cx--;
			} else if (e.cy > 0) {
				e.cy--;
				e.cx = (int)kstrlen(e.line[e.cy]);
			}
			break;
		case KEY_RIGHT:
			if (e.line[e.cy][e.cx]) {
				e.cx++;
			} else if (e.cy + 1 < e.nlines) {
				e.cy++;
				e.cx = 0;
			}
			break;
		case KEY_HOME:  e.cx = 0; break;
		case KEY_END:   e.cx = (int)kstrlen(e.line[e.cy]); break;
		case KEY_DEL:   delete_char(&e); break;
		case '\b':      backspace(&e); break;
		case '\n':      newline(&e); break;
		case '\t':      insert_char(&e, ' '); insert_char(&e, ' '); break;
		default:
			if (k >= 32 && k < 127)
				insert_char(&e, (char)k);
			break;
		}
	}
	kfree(e.line);
	console_clear();
	return 0;
}
