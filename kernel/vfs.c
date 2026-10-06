#include "vfs.h"

#include "console.h"
#include "kstring.h"

struct mount {
	char prefix[VFS_NAME_MAX]; /* "/" or "/dev": no trailing slash except for the root */
	const struct vfs_ops *ops;
	void *ctx;
	int used;
};

static struct mount mounts[VFS_MAX_MOUNTS];

/* ---- TinyFS adapter: a flat namespace, so only the root directory exists ---- */

static const char *tfs_name(const char *path)
{
	while (*path == '/')
		path++;
	return path;
}

static int tfs_stat(void *ctx, const char *path, struct vfs_stat *st)
{
	int size;

	(void)ctx;
	path = tfs_name(path);
	if (!*path) { /* the root directory itself */
		st->size = 0;
		st->is_dir = 1;
		return FS_OK;
	}
	size = fs_size(path);
	if (size < 0)
		return size;
	st->size = (uint32_t)size;
	st->is_dir = 0;
	return FS_OK;
}

static int tfs_read(void *ctx, const char *path, void *buf, uint32_t cap)
{
	(void)ctx;
	return fs_read(tfs_name(path), buf, cap);
}

static int tfs_write(void *ctx, const char *path, const void *data, uint32_t size)
{
	(void)ctx;
	return fs_write(tfs_name(path), data, size);
}

static int tfs_create(void *ctx, const char *path)
{
	(void)ctx;
	return fs_create(tfs_name(path));
}

static int tfs_unlink(void *ctx, const char *path)
{
	(void)ctx;
	return fs_delete(tfs_name(path));
}

static int tfs_list(void *ctx, const char *path, struct vfs_dirent *out, int max)
{
	struct fs_stat st[FS_MAX_FILES];
	int n, i;

	(void)ctx;
	if (*tfs_name(path))
		return FS_ENOENT; /* no subdirectories */
	n = fs_list(st, FS_MAX_FILES);
	if (n < 0)
		return n;
	for (i = 0; i < n && i < max; i++) {
		kstrlcpy(out[i].name, st[i].name, sizeof out[i].name);
		out[i].size = st[i].size;
		out[i].is_dir = 0;
	}
	return i;
}

static const struct vfs_ops tinyfs_ops = {
	"tinyfs", tfs_stat, tfs_read, tfs_write, tfs_create, tfs_unlink, tfs_list,
};

/* ---- mount table ---- */

void vfs_init(void)
{
	memset(mounts, 0, sizeof mounts);
	vfs_mount("/", &tinyfs_ops, 0);
}

int vfs_mount(const char *prefix, const struct vfs_ops *ops, void *ctx)
{
	int i;

	if (prefix[0] != '/' || kstrlen(prefix) >= VFS_NAME_MAX)
		return FS_EINVAL;
	for (i = 0; i < VFS_MAX_MOUNTS; i++)
		if (mounts[i].used && !kstrcmp(mounts[i].prefix, prefix))
			return FS_EEXIST;
	for (i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (!mounts[i].used) {
			kstrlcpy(mounts[i].prefix, prefix, sizeof mounts[i].prefix);
			mounts[i].ops = ops;
			mounts[i].ctx = ctx;
			mounts[i].used = 1;
			return FS_OK;
		}
	}
	return FS_ENOSPC;
}

int vfs_unmount(const char *prefix)
{
	int i;

	for (i = 0; i < VFS_MAX_MOUNTS; i++) {
		if (mounts[i].used && !kstrcmp(mounts[i].prefix, prefix)) {
			mounts[i].used = 0;
			return FS_OK;
		}
	}
	return FS_ENOENT;
}

/* Longest mount prefix that matches on a path-component boundary wins. */
static struct mount *resolve(const char *path, const char **rest)
{
	struct mount *best = 0;
	size_t bestlen = 0;
	int i;

	for (i = 0; i < VFS_MAX_MOUNTS; i++) {
		size_t len;

		if (!mounts[i].used)
			continue;
		len = kstrlen(mounts[i].prefix);
		if (len == 1) /* root matches everything */
			len = 0;
		if (kstrncmp(path, mounts[i].prefix, len) == 0 && (path[len] == '/' || path[len] == '\0')
		    && (!best || len >= bestlen)) {
			best = &mounts[i];
			bestlen = len;
		}
	}
	if (best)
		*rest = path + bestlen;
	return best;
}

/* Paths without a leading slash are relative to the root (TinyFS has no directories yet). */
static struct mount *lookup(const char *path, char *full, size_t size, const char **rest)
{
	if (path[0] != '/') {
		full[0] = '/';
		kstrlcpy(full + 1, path, size - 1);
	} else {
		kstrlcpy(full, path, size);
	}
	return resolve(full, rest);
}

int vfs_stat(const char *path, struct vfs_stat *st)
{
	char full[64];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->stat ? m->ops->stat(m->ctx, rest, st) : FS_ENOENT;
}

int vfs_size(const char *path)
{
	struct vfs_stat st;
	int rc = vfs_stat(path, &st);

	if (rc < 0)
		return rc;
	return st.is_dir ? FS_EINVAL : (int)st.size;
}

int vfs_read(const char *path, void *buf, uint32_t cap)
{
	char full[64];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->read ? m->ops->read(m->ctx, rest, buf, cap) : FS_ENOENT;
}

int vfs_write(const char *path, const void *data, uint32_t size)
{
	char full[64];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->write ? m->ops->write(m->ctx, rest, data, size) : FS_ENOENT;
}

int vfs_create(const char *path)
{
	char full[64];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->create ? m->ops->create(m->ctx, rest) : FS_ENOENT;
}

int vfs_unlink(const char *path)
{
	char full[64];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->unlink ? m->ops->unlink(m->ctx, rest) : FS_ENOENT;
}

int vfs_list(const char *path, struct vfs_dirent *out, int max)
{
	char full[64];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->list ? m->ops->list(m->ctx, rest, out, max) : FS_ENOENT;
}

void vfs_print_mounts(void)
{
	int i;

	for (i = 0; i < VFS_MAX_MOUNTS; i++)
		if (mounts[i].used)
			console_printf("  %-10s on %s\n", mounts[i].ops->name, mounts[i].prefix);
}
