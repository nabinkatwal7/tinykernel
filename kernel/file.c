#include "file.h"

#include "console.h"
#include "vfs.h"
#include "io.h"
#include "keyboard.h"
#include "pipe.h"
#include "kmalloc.h"
#include "kstring.h"

/*
 * Two layers, like Unix: the fd table maps small integers to open-file objects, and several fds
 * (after dup/dup2) may share one object - and with it the cursor. Objects are either the
 * console (stdin/stdout/stderr) or a buffered regular file.
 */
enum { OBJ_FREE, OBJ_CONSOLE_IN, OBJ_CONSOLE_OUT, OBJ_FILE, OBJ_DEV, OBJ_PIPE_R, OBJ_PIPE_W };

struct ofile {
	int kind;
	int refs;
	int flags;
	char name[FILE_PATH_MAX];
	uint8_t *buf;
	uint32_t size, cap, pos;
	int dirty;
	const struct vfs_device *dev; /* OBJ_DEV */
	struct pipe *pipe;            /* OBJ_PIPE_R / OBJ_PIPE_W */
};

#define NOBJ (FILE_MAX_OPEN + 3 + 8) /* files, the console, and a few pipe ends */

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
	if (o->kind == OBJ_PIPE_R)
		pipe_close_read(o->pipe);
	else if (o->kind == OBJ_PIPE_W)
		pipe_close_write(o->pipe);
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
	struct vfs_stat st;
	int oi, fd, size, rc;

	if (kstrlen(path) >= FILE_PATH_MAX - 1)
		return FS_EINVAL;
	fd = new_fd();
	oi = new_obj();
	if (fd < 0 || oi < 0)
		return FS_ENOSPC;

	rc = vfs_stat(path, &st);
	if (rc == FS_OK && st.dev) { /* device file: no buffering, every read/write goes to the driver */
		o = &objs[oi];
		memset(o, 0, sizeof *o);
		o->kind = OBJ_DEV;
		o->dev = st.dev;
		o->flags = flags;
		o->refs = 1;
		fdtab[fd] = oi;
		return fd;
	}
	if (rc == FS_OK && st.is_dir)
		return FS_EINVAL;
	if (writable(flags) && !vfs_can_write(path))
		return FS_EROFS; /* read-only mount (FAT12, /proc) */
	size = rc == FS_OK ? (int)st.size : rc;
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

int file_pipe(int fds[2])
{
	struct pipe *p;
	int r = new_fd(), w, ro, wo;

	if (r < 0)
		return FS_ENOSPC;
	fdtab[r] = 0; /* claim it while looking for the second descriptor */
	w = new_fd();
	fdtab[r] = -1;
	ro = new_obj();
	if (ro >= 0)
		objs[ro].kind = OBJ_PIPE_R; /* claim it too */
	wo = new_obj();
	if (w < 0 || ro < 0 || wo < 0) {
		if (ro >= 0)
			objs[ro].kind = OBJ_FREE;
		return FS_ENOSPC;
	}
	p = pipe_new();
	if (!p) {
		objs[ro].kind = OBJ_FREE;
		return FS_ENOSPC;
	}
	memset(&objs[ro], 0, sizeof objs[ro]);
	objs[ro].kind = OBJ_PIPE_R;
	objs[ro].refs = 1;
	objs[ro].pipe = p;
	objs[wo].kind = OBJ_PIPE_W;
	objs[wo].refs = 1;
	objs[wo].flags = O_WRONLY;
	objs[wo].pipe = p;
	fdtab[r] = ro;
	fdtab[w] = wo;
	fds[0] = r;
	fds[1] = w;
	return 0;
}

int file_seek(int fd, int32_t off, int whence)
{
	struct ofile *o = get(fd);
	int64_t base, target;

	if (!o || o->kind != OBJ_FILE)
		return FS_EINVAL;
	switch (whence) {
	case SEEK_SET: base = 0; break;
	case SEEK_CUR: base = o->pos; break;
	case SEEK_END: base = o->size; break;
	default: return FS_EINVAL;
	}
	target = base + off;
	if (target < 0 || target > FILE_MAX_SIZE)
		return FS_EINVAL;
	o->pos = (uint32_t)target; /* may lie past the end: the gap is zero-filled on the next write */
	return (int)o->pos;
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

int console_stdin_read(void *buf, uint32_t n)
{
	uint32_t got = 0;

	if (line_pos >= line_len)
		read_console_line();
	while (got < n && line_pos < line_len)
		((char *)buf)[got++] = linebuf[line_pos++];
	return (int)got;
}

int file_read(int fd, void *buf, uint32_t n)
{
	struct ofile *o = get(fd);

	if (!o)
		return FS_EINVAL;
	if (o->kind == OBJ_CONSOLE_IN)
		return console_stdin_read(buf, n);
	if (o->kind == OBJ_DEV) {
		if ((o->flags & O_WRONLY) || !o->dev->read)
			return FS_EINVAL;
		return o->dev->read(buf, n);
	}
	if (o->kind == OBJ_PIPE_R)
		return pipe_read(o->pipe, buf, n);
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
	if (o->kind == OBJ_DEV) {
		if (!writable(o->flags) || !o->dev->write)
			return FS_EINVAL;
		return o->dev->write(buf, n);
	}
	if (o->kind == OBJ_PIPE_W) {
		int rc = pipe_write(o->pipe, buf, n);

		return rc < 0 ? FS_EPIPE : rc;
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
	if (o->pos > o->size) /* seeked past the end: the hole reads as zeros */
		memset(o->buf + o->size, 0, o->pos - o->size);
	memcpy(o->buf + o->pos, buf, n);
	o->pos += n;
	if (o->pos > o->size)
		o->size = o->pos;
	o->dirty = 1;
	return (int)n;
}
