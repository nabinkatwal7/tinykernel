#include "file.h"

#include "fs.h"
#include "io.h"
#include "kmalloc.h"
#include "kstring.h"

struct ofile {
	int used;
	int flags;
	char name[FS_NAME_MAX];
	uint8_t *buf;
	uint32_t size, cap, pos;
	int dirty;
};

static struct ofile table[FILE_MAX_OPEN];

static struct ofile *lookup(int fd)
{
	if (fd < FILE_FIRST_FD || fd >= FILE_FIRST_FD + FILE_MAX_OPEN)
		return 0;
	return table[fd - FILE_FIRST_FD].used ? &table[fd - FILE_FIRST_FD] : 0;
}

static int writable(int flags)
{
	return flags & (O_WRONLY | O_RDWR);
}

int file_open(const char *path, int flags)
{
	struct ofile *f = 0;
	int i, size, rc;

	if (!fs_mounted() && fs_mount() != FS_OK)
		return FS_ENOMOUNT;
	for (i = 0; i < FILE_MAX_OPEN; i++) {
		if (!table[i].used) {
			f = &table[i];
			break;
		}
	}
	if (!f)
		return FS_ENOSPC;
	if (kstrlen(path) >= FS_NAME_MAX)
		return FS_EINVAL;

	size = fs_size(path);
	if (size == FS_ENOENT) {
		if (!(flags & O_CREAT))
			return FS_ENOENT;
		rc = fs_create(path); /* makes it visible immediately, like creat() */
		if (rc)
			return rc;
		size = 0;
	} else if (size < 0) {
		return size;
	}
	if (size > FILE_MAX_SIZE)
		return FS_ETOOBIG;

	memset(f, 0, sizeof *f);
	f->flags = flags;
	kstrlcpy(f->name, path, sizeof f->name);
	f->cap = (uint32_t)size + 64;
	f->buf = kmalloc(f->cap);
	if (!f->buf)
		return FS_ENOSPC;
	if (size > 0 && !(writable(flags) && (flags & O_TRUNC))) {
		rc = fs_read(path, f->buf, (uint32_t)size);
		if (rc < 0) {
			kfree(f->buf);
			return rc;
		}
		f->size = (uint32_t)size;
	}
	if (writable(flags) && (flags & O_TRUNC) && size > 0)
		f->dirty = 1; /* truncation itself is a change */
	f->pos = (flags & O_APPEND) ? f->size : 0;
	f->used = 1;
	return (int)(f - table) + FILE_FIRST_FD;
}

int file_close(int fd)
{
	struct ofile *f = lookup(fd);
	int rc = FS_OK;

	if (!f)
		return FS_EINVAL;
	if (writable(f->flags) && f->dirty)
		rc = fs_write(f->name, f->buf, f->size);
	kfree(f->buf);
	memset(f, 0, sizeof *f);
	return rc;
}

void file_close_all(void)
{
	int i;

	for (i = 0; i < FILE_MAX_OPEN; i++)
		if (table[i].used)
			file_close(i + FILE_FIRST_FD);
}

int file_open_count(void)
{
	int i, n = 0;

	for (i = 0; i < FILE_MAX_OPEN; i++)
		n += table[i].used;
	return n;
}

int file_read(int fd, void *buf, uint32_t n)
{
	struct ofile *f = lookup(fd);

	if (!f)
		return FS_EINVAL;
	if (f->flags & O_WRONLY)
		return FS_EINVAL;
	if (f->pos >= f->size)
		return 0; /* end of file */
	if (n > f->size - f->pos)
		n = f->size - f->pos;
	memcpy(buf, f->buf + f->pos, n);
	f->pos += n;
	return (int)n;
}

int file_write(int fd, const void *buf, uint32_t n)
{
	struct ofile *f = lookup(fd);

	if (!f || !writable(f->flags))
		return FS_EINVAL;
	if (f->pos + n > FILE_MAX_SIZE)
		return FS_ENOSPC;
	if (f->pos + n > f->cap) { /* grow geometrically */
		uint32_t cap = f->cap * 2;
		uint8_t *nb;

		if (cap < f->pos + n)
			cap = f->pos + n;
		nb = kmalloc(cap);
		if (!nb)
			return FS_ENOSPC;
		memcpy(nb, f->buf, f->size);
		kfree(f->buf);
		f->buf = nb;
		f->cap = cap;
	}
	if (f->pos > f->size) /* never happens without seek, but keep holes zeroed */
		memset(f->buf + f->size, 0, f->pos - f->size);
	memcpy(f->buf + f->pos, buf, n);
	f->pos += n;
	if (f->pos > f->size)
		f->size = f->pos;
	f->dirty = 1;
	return (int)n;
}
