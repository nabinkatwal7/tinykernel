#include "vfs.h"

#include "console.h"
#include "kprintf.h"
#include "kmalloc.h"
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

/* Mount the disk on first use so a freshly booted system just works. */
static void ensure_mounted(void)
{
	if (!fs_mounted())
		fs_mount();
}

static int tfs_stat(void *ctx, const char *path, struct vfs_stat *st)
{
	struct fs_stat fst;
	int rc;

	(void)ctx;
	ensure_mounted();
	st->dev = 0;
	rc = fs_stat(tfs_name(path), &fst);
	if (rc < 0)
		return rc;
	st->size = fst.size;
	st->is_dir = fst.is_dir;
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
	struct fs_stat *st = kmalloc(FS_MAX_FILES * sizeof *st);
	int n, i;

	(void)ctx;
	if (!st)
		return FS_ENOSPC;
	ensure_mounted();
	n = fs_list(tfs_name(path), st, FS_MAX_FILES);
	for (i = 0; i < n && i < max; i++) {
		kstrlcpy(out[i].name, st[i].name, sizeof out[i].name);
		out[i].size = st[i].size;
		out[i].is_dir = st[i].is_dir;
	}
	kfree(st);
	return n < 0 ? n : i;
}

static int tfs_mkdir(void *ctx, const char *path)
{
	(void)ctx;
	ensure_mounted();
	return fs_mkdir(tfs_name(path));
}

static int tfs_rmdir(void *ctx, const char *path)
{
	(void)ctx;
	ensure_mounted();
	return fs_rmdir(tfs_name(path));
}

static int tfs_rename(void *ctx, const char *from, const char *to)
{
	(void)ctx;
	ensure_mounted();
	return fs_rename(tfs_name(from), tfs_name(to));
}

static const struct vfs_ops tinyfs_ops = {
	"tinyfs", tfs_stat, tfs_read, tfs_write, tfs_create, tfs_unlink, tfs_list, tfs_mkdir, tfs_rmdir, tfs_rename,
};

/* ---- mount table ---- */

void vfs_init(void)
{
	memset(mounts, 0, sizeof mounts);
	vfs_mount("/", &tinyfs_ops, 0);
	devfs_init();
	procfs_init();
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

/* Current working directory: always an absolute path with no trailing slash (except "/"). */
static char cwd[VFS_PATH_MAX] = "/";

int vfs_normalize(const char *base, const char *path, char *out, uint32_t size)
{
	/* Components of base (if path is relative) followed by those of path, as (start,len) pairs. */
	struct { const char *p; uint32_t n; } comp[VFS_PATH_MAX];
	int depth = 0;
	uint32_t o = 0;
	int pass, i;

	for (pass = 0; pass < 2; pass++) {
		const char *s = pass == 0 ? (path[0] == '/' ? "" : base) : path;

		for (;;) {
			uint32_t n = 0;

			while (*s == '/')
				s++;
			if (!*s)
				break;
			while (s[n] && s[n] != '/')
				n++;
			if (n == 1 && s[0] == '.') {
				/* current directory: nothing */
			} else if (n == 2 && s[0] == '.' && s[1] == '.') {
				if (depth)
					depth--; /* at the root, '..' stays at the root */
			} else if (depth < VFS_PATH_MAX) {
				comp[depth].p = s;
				comp[depth].n = n;
				depth++;
			} else {
				return FS_EINVAL;
			}
			s += n;
		}
	}
	if (size < 2)
		return FS_EINVAL;
	out[o++] = '/';
	for (i = 0; i < depth; i++) {
		if (o + comp[i].n + 1 >= size)
			return FS_EINVAL;
		memcpy(out + o, comp[i].p, comp[i].n);
		o += comp[i].n;
		if (i + 1 < depth)
			out[o++] = '/';
	}
	out[o] = '\0';
	return FS_OK;
}

/* Absolute, normalized path for 'path' (relative paths start at the cwd). Falls back to the
   unnormalized text if it is too long, so the lookup fails instead of hitting the wrong file. */
static void make_absolute(const char *path, char *full, size_t size)
{
	if (vfs_normalize(cwd, path, full, (uint32_t)size))
		kstrlcpy(full, "/?", size); /* no such mount/file */
}

static struct mount *lookup(const char *path, char *full, size_t size, const char **rest)
{
	make_absolute(path, full, size);
	return resolve(full, rest);
}

const char *vfs_getcwd(void)
{
	return cwd;
}

int vfs_chdir(const char *path)
{
	char full[VFS_PATH_MAX];
	struct vfs_stat st;
	int rc;
	size_t n;

	make_absolute(path, full, sizeof full);
	rc = vfs_stat(full, &st);
	if (rc < 0)
		return rc;
	if (!st.is_dir)
		return FS_ENOTDIR;
	n = kstrlen(full);
	while (n > 1 && full[n - 1] == '/') /* no trailing slash */
		full[--n] = '\0';
	kstrlcpy(cwd, full, sizeof cwd);
	return FS_OK;
}

int vfs_stat(const char *path, struct vfs_stat *st)
{
	char full[VFS_PATH_MAX];
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
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->read ? m->ops->read(m->ctx, rest, buf, cap) : FS_ENOENT;
}

int vfs_write(const char *path, const void *data, uint32_t size)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->write ? m->ops->write(m->ctx, rest, data, size) : FS_ENOENT;
}

int vfs_create(const char *path)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->create ? m->ops->create(m->ctx, rest) : FS_ENOENT;
}

int vfs_unlink(const char *path)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->unlink ? m->ops->unlink(m->ctx, rest) : FS_ENOENT;
}

int vfs_rename(const char *from, const char *to)
{
	char f1[VFS_PATH_MAX], f2[VFS_PATH_MAX];
	const char *r1, *r2;
	struct mount *m1 = lookup(from, f1, sizeof f1, &r1);
	struct mount *m2 = lookup(to, f2, sizeof f2, &r2);

	if (!m1 || m1 != m2 || !m1->ops->rename)
		return FS_EINVAL;
	return m1->ops->rename(m1->ctx, r1, r2);
}

int vfs_mkdir(const char *path)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->mkdir ? m->ops->mkdir(m->ctx, rest) : FS_EINVAL;
}

int vfs_rmdir(const char *path)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->rmdir ? m->ops->rmdir(m->ctx, rest) : FS_EINVAL;
}

int vfs_list(const char *path, struct vfs_dirent *out, int max)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->list ? m->ops->list(m->ctx, rest, out, max) : FS_ENOENT;
}

/* Mount table as text, for /proc/mounts. */
int vfs_format_mounts(char *buf, uint32_t cap)
{
	int i, n = 0;

	for (i = 0; i < VFS_MAX_MOUNTS; i++)
		if (mounts[i].used)
			n += ksnprintf(buf + n, cap - (uint32_t)n, "%s %s\n", mounts[i].ops->name, mounts[i].prefix);
	return n;
}

void vfs_print_mounts(void)
{
	int i;

	for (i = 0; i < VFS_MAX_MOUNTS; i++)
		if (mounts[i].used)
			console_printf("  %-10s on %s\n", mounts[i].ops->name, mounts[i].prefix);
}
