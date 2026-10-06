#include "file.h"

#include "console.h"
#include "vfs.h"
#include "io.h"
#include "keyboard.h"
#include "kmalloc.h"
#include "kstring.h"

/*
 * Two layers, like Unix: the fd table maps small integers to open-file objects, and several fds
 * (after dup/dup2) may share one object - and with it the cursor. Objects are either the
 * console (stdin/stdout/stderr) or a buffered regular file.
 */
enum { OBJ_FREE, OBJ_CONSOLE_IN, OBJ_CONSOLE_OUT, OBJ_FILE };

struct ofile {
	int kind;
	int refs;
	int flags;
	char name[FS_NAME_MAX];
	uint8_t *buf;
	uint32_t size, cap, pos;
	int dirty;
};

#define NOBJ (FILE_MAX_OPEN + 3)

static struct ofile objs[NOBJ];
static int fdtab[FILE_FD_MAX]; /* index into objs, or -1 */

/* cooked console input: one line is gathered at a time and handed out in pieces */
static char linebuf[128];
static int line_len, line_pos;

static int writable(int flags)
{
	return flags & (O_WRONLY | O_RDWR);
}

static struct ofile *get(int fd)
{
	if (fd < 0 || fd >= FILE_FD_MAX || fdtab[fd] < 0)
		return 0;
	return &objs[fdtab[fd]];
}

static int new_obj(void)
{
	int i;

	for (i = 0; i < NOBJ; i++)
		if (objs[i].kind == OBJ_FREE)
			return i;
	return -1;
}

static int new_fd(void)
{
	int fd;

	for (fd = 0; fd < FILE_FD_MAX; fd++)
		if (fdtab[fd] < 0)
			return fd;
	return -1;
}

/* Drop one reference; the last one flushes and frees a regular file. */
static int release(struct ofile *o)
{
	int rc = FS_OK;

	if (--o->refs > 0)
		return rc;
	if (o->kind == OBJ_FILE) {
		if (writable(o->flags) && o->dirty)
			rc = vfs_write(o->name, o->buf, o->size);
		kfree(o->buf);
	}
	memset(o, 0, sizeof *o);
	return rc;
}

void file_reset(void)
{
	int fd;

	for (fd = 0; fd < FILE_FD_MAX; fd++) {
		if (fdtab[fd] >= 0 && objs[fdtab[fd]].kind != OBJ_FREE)
			release(&objs[fdtab[fd]]);
		fdtab[fd] = -1;
	}
	memset(objs, 0, sizeof objs);
	objs[0].kind = OBJ_CONSOLE_IN;
	objs[1].kind = OBJ_CONSOLE_OUT;
	objs[2].kind = OBJ_CONSOLE_OUT;
	objs[0].refs = objs[1].refs = objs[2].refs = 1;
	fdtab[0] = 0;
	fdtab[1] = 1;
	fdtab[2] = 2;
	line_len = line_pos = 0;
}

void file_close_all(void)
{
	file_reset();
}

int file_open_count(void)
{
	int fd, n = 0;

	for (fd = FILE_FIRST_FD; fd < FILE_FD_MAX; fd++)
		n += fdtab[fd] >= 0;
	return n;
}

int file_open(const char *path, int flags)
{
	struct ofile *o;
	int oi, fd, size, rc;

	if (!fs_mounted() && fs_mount() != FS_OK)
		return FS_ENOMOUNT;
	if (kstrlen(path) >= FS_NAME_MAX)
		return FS_EINVAL;
	fd = new_fd();
	oi = new_obj();
	if (fd < 0 || oi < 0)
		return FS_ENOSPC;

	size = vfs_size(path);
	if (size == FS_ENOENT) {
		if (!(flags & O_CREAT))
			return FS_ENOENT;
		rc = vfs_create(path); /* visible immediately, like creat() */
		if (rc)
			return rc;
		size = 0;
	} else if (size < 0) {
		return size;
	}
	if (size > FILE_MAX_SIZE)
		return FS_ETOOBIG;

	o = &objs[oi];
	memset(o, 0, sizeof *o);
	o->kind = OBJ_FILE;
	o->flags = flags;
	kstrlcpy(o->name, path, sizeof o->name);
	o->cap = (uint32_t)size + 64;
	o->buf = kmalloc(o->cap);
	if (!o->buf) {
		o->kind = OBJ_FREE;
		return FS_ENOSPC;
	}
	if (size > 0 && !(writable(flags) && (flags & O_TRUNC))) {
		rc = vfs_read(path, o->buf, (uint32_t)size);
		if (rc < 0) {
			kfree(o->buf);
			o->kind = OBJ_FREE;
			return rc;
		}
		o->size = (uint32_t)size;
	}
	if (writable(flags) && (flags & O_TRUNC) && size > 0)
		o->dirty = 1; /* truncation itself is a change */
	o->pos = (flags & O_APPEND) ? o->size : 0;
	o->refs = 1;
	fdtab[fd] = oi;
	return fd;
}

int file_close(int fd)
{
	struct ofile *o = get(fd);

	if (!o)
		return FS_EINVAL;
	fdtab[fd] = -1;
	return release(o);
}

int file_dup(int fd)
{
	struct ofile *o = get(fd);
	int nfd = new_fd();

	if (!o || nfd < 0)
		return FS_EINVAL;
	o->refs++;
	fdtab[nfd] = fdtab[fd];
	return nfd;
}

int file_dup2(int fd, int target)
{
	struct ofile *o = get(fd);

	if (!o || target < 0 || target >= FILE_FD_MAX)
		return FS_EINVAL;
	if (fd == target)
		return target;
	if (fdtab[target] >= 0)
		file_close(target);
	o->refs++;
	fdtab[target] = fdtab[fd];
	return target;
}

/* Fill the line buffer: a tiny cooked-mode terminal with echo and backspace. */
static void read_console_line(void)
{
	line_len = line_pos = 0;
	for (;;) {
		int k = keyboard_getkey();

		if (k == '\n') {
			console_putchar('\n');
			linebuf[line_len++] = '\n';
			return;
		}
		if (k == '\b') {
			if (line_len) {
				line_len--;
				console_putchar('\b');
			}
		} else if (k >= 32 && k < 127 && line_len < (int)sizeof linebuf - 1) {
			linebuf[line_len++] = (char)k;
			console_putchar((char)k);
		}
	}
}

int file_read(int fd, void *buf, uint32_t n)
{
	struct ofile *o = get(fd);

	if (!o)
		return FS_EINVAL;
	if (o->kind == OBJ_CONSOLE_IN) {
		uint32_t got = 0;

		if (line_pos >= line_len)
			read_console_line();
		while (got < n && line_pos < line_len)
			((char *)buf)[got++] = linebuf[line_pos++];
		return (int)got;
	}
	if (o->kind != OBJ_FILE || (o->flags & O_WRONLY))
		return FS_EINVAL;
	if (o->pos >= o->size)
		return 0; /* end of file */
	if (n > o->size - o->pos)
		n = o->size - o->pos;
	memcpy(buf, o->buf + o->pos, n);
	o->pos += n;
	return (int)n;
}

int file_write(int fd, const void *buf, uint32_t n)
{
	struct ofile *o = get(fd);

	if (!o)
		return FS_EINVAL;
	if (o->kind == OBJ_CONSOLE_OUT) {
		uint32_t i;

		for (i = 0; i < n; i++)
			console_putchar(((const char *)buf)[i]);
		return (int)n;
	}
	if (o->kind != OBJ_FILE || !writable(o->flags))
		return FS_EINVAL;
	if (o->pos + n > FILE_MAX_SIZE)
		return FS_ENOSPC;
	if (o->pos + n > o->cap) { /* grow geometrically */
		uint32_t cap = o->cap * 2;
		uint8_t *nb;

		if (cap < o->pos + n)
			cap = o->pos + n;
		nb = kmalloc(cap);
		if (!nb)
			return FS_ENOSPC;
		memcpy(nb, o->buf, o->size);
		kfree(o->buf);
		o->buf = nb;
		o->cap = cap;
	}
	memcpy(o->buf + o->pos, buf, n);
	o->pos += n;
	if (o->pos > o->size)
		o->size = o->pos;
	o->dirty = 1;
	return (int)n;
}
