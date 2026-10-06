#include "vfs.h"

#include "console.h"
#include "kprintf.h"
#include "fat12.h"
#include "kmalloc.h"
#include "pcache.h"
#include "kstring.h"
#include "cred.h"

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
	st->has_meta = 1;
	st->is_link = 0;
	if (!*tfs_name(path)) { /* the root always exists, even on an unformatted disk */
		st->size = 0;
		st->is_dir = 1;
		st->mode = 0755;
		st->uid = st->gid = 0;
		st->mtime = 0;
		return FS_OK;
	}
	rc = fs_stat(tfs_name(path), &fst);
	if (rc < 0)
		return rc;
	st->size = fst.size;
	st->is_dir = fst.is_dir;
	st->is_link = fst.is_link;
	st->nlink = fst.nlink;
	st->mode = fst.meta.mode;
	st->uid = fst.meta.uid;
	st->gid = fst.meta.gid;
	st->mtime = fst.meta.mtime;
	st->ctime = fst.meta.ctime;
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
		out[i].is_link = st[i].is_link;
		out[i].has_meta = 1;
		out[i].mode = st[i].meta.mode;
		out[i].uid = st[i].meta.uid;
		out[i].gid = st[i].meta.gid;
		out[i].mtime = st[i].meta.mtime;
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

static int tfs_symlink(void *ctx, const char *target, const char *path)
{
	(void)ctx;
	ensure_mounted();
	return fs_symlink(target, tfs_name(path));
}

static int tfs_readlink(void *ctx, const char *path, char *buf, uint32_t cap)
{
	(void)ctx;
	ensure_mounted();
	return fs_readlink(tfs_name(path), buf, cap);
}

static int tfs_link(void *ctx, const char *existing, const char *path)
{
	(void)ctx;
	ensure_mounted();
	return fs_link(tfs_name(existing), tfs_name(path));
}

static int tfs_chmod(void *ctx, const char *path, uint16_t mode)
{
	(void)ctx;
	ensure_mounted();
	return fs_chmod(tfs_name(path), mode);
}

static int tfs_chown(void *ctx, const char *path, uint16_t uid, uint16_t gid)
{
	(void)ctx;
	ensure_mounted();
	return fs_chown(tfs_name(path), uid, gid);
}

static int tfs_touch(void *ctx, const char *path, uint32_t mtime)
{
	(void)ctx;
	ensure_mounted();
	return fs_touch(tfs_name(path), mtime);
}

static int tfs_rename(void *ctx, const char *from, const char *to)
{
	(void)ctx;
	ensure_mounted();
	return fs_rename(tfs_name(from), tfs_name(to));
}

/* ---- FAT12 (read-only), mounted on /fat; the IDE slave is attached on first use ---- */

static int fat_ready(void)
{
	return fat12_mounted() || fat12_mount(1) == FS_OK ? FS_OK : FS_ENOMOUNT;
}

static int fatv_stat(void *ctx, const char *path, struct vfs_stat *st)
{
	struct fat12_entry e;
	int rc = fat_ready();

	(void)ctx;
	if (rc)
		return rc;
	rc = fat12_stat(path, &e);
	if (rc < 0)
		return rc;
	st->size = e.size;
	st->is_dir = e.is_dir;
	st->dev = 0;
	return FS_OK;
}

static int fatv_read(void *ctx, const char *path, void *buf, uint32_t cap)
{
	(void)ctx;
	return fat_ready() ? FS_ENOMOUNT : fat12_read(path, buf, cap);
}

static int fatv_list(void *ctx, const char *path, struct vfs_dirent *out, int max)
{
	struct fat12_entry *ents;
	int n, i;

	(void)ctx;
	if (fat_ready())
		return FS_ENOMOUNT;
	ents = kmalloc(64 * sizeof *ents);
	if (!ents)
		return FS_ENOSPC;
	n = fat12_list(path, ents, 64);
	for (i = 0; i < n && i < max; i++) {
		kstrlcpy(out[i].name, ents[i].name, sizeof out[i].name);
		out[i].size = ents[i].size;
		out[i].is_dir = ents[i].is_dir;
	}
	kfree(ents);
	return n < 0 ? n : i;
}

static const struct vfs_ops fat_ops = {
	"fat12", fatv_stat, fatv_read, 0, 0, 0, fatv_list, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const struct vfs_ops tinyfs_ops = {
	"tinyfs", tfs_stat, tfs_read, tfs_write, tfs_create, tfs_unlink, tfs_list, tfs_mkdir, tfs_rmdir, tfs_rename,
	tfs_chmod, tfs_chown, tfs_touch, tfs_symlink, tfs_readlink, tfs_link,
};

/* ---- mount table ---- */

void vfs_init(void)
{
	memset(mounts, 0, sizeof mounts);
	vfs_mount("/", &tinyfs_ops, 0);
	vfs_mount("/fat", &fat_ops, 0);
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

/* Target of the symbolic link at the absolute path 'abs' on TinyFS, or a negative value if it is not one. */
static int link_target(const char *abs, char *target, uint32_t cap)
{
	const char *rest;
	struct mount *m = resolve(abs, &rest);

	if (!m || m->ops != &tinyfs_ops || !*tfs_name(rest))
		return FS_EINVAL;
	return tfs_readlink(0, rest, target, cap);
}

#define MAX_LINK_HOPS 8

/*
 * Rewrites 'full' (absolute, normalized) so that no component is a symbolic link: the first link found is
 * replaced by its target followed by the rest of the path, and the scan starts again. The last component is
 * left alone unless follow_last is set. Too many hops (a loop) leave the path unusable ("/?").
 */
static void follow_links(char *full, size_t size, int follow_last)
{
	int hops;

	for (hops = 0; hops <= MAX_LINK_HOPS; hops++) {
		char target[VFS_PATH_MAX], joined[VFS_PATH_MAX], dirpart[VFS_PATH_MAX];
		size_t pos;
		int changed = 0;

		for (pos = 1; full[pos - 1] && !changed; pos++) {
			char saved = full[pos];
			size_t slash;

			if (saved != '/' && saved != '\0')
				continue;
			if (!saved && !follow_last)
				break;
			full[pos] = '\0';
			if (link_target(full, target, sizeof target) >= 0) {
				for (slash = pos; slash > 0 && full[slash] != '/'; slash--)
					;
				memcpy(dirpart, full, slash ? slash : 1);
				dirpart[slash ? slash : 1] = '\0';
				full[pos] = saved;
				if (vfs_normalize(dirpart, target, joined, sizeof joined)) {
					kstrlcpy(full, "/?", size);
					return;
				}
				if (kstrlen(joined) + kstrlen(full + pos) + 1 > size) {
					kstrlcpy(full, "/?", size);
					return;
				}
				kstrlcpy(dirpart, joined, sizeof dirpart);
				kstrlcpy(joined, dirpart, sizeof joined);
				memcpy(joined + kstrlen(joined), full + pos, kstrlen(full + pos) + 1);
				if (vfs_normalize("/", joined, full, (uint32_t)size))
					kstrlcpy(full, "/?", size);
				changed = 1;
			} else {
				full[pos] = saved;
				if (!saved)
					break;
			}
		}
		if (!changed)
			return;
	}
	kstrlcpy(full, "/?", size); /* a loop of links */
}

/* Like lookup(), but a final symbolic link is not followed (links in the middle always are). */
static struct mount *lookup_nofollow(const char *path, char *full, size_t size, const char **rest)
{
	make_absolute(path, full, size);
	follow_links(full, size, 0);
	return resolve(full, rest);
}

static struct mount *lookup(const char *path, char *full, size_t size, const char **rest)
{
	make_absolute(path, full, size);
	follow_links(full, size, 1);
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
	int rc;

	if (!m || !m->ops->stat)
		return FS_ENOENT;
	memset(st, 0, sizeof *st);
	rc = m->ops->stat(m->ctx, rest, st);
	if (rc >= 0 && !st->has_meta) { /* devfs, procfs, FAT: root-owned, world-readable */
		st->mode = st->dev ? 0666 : st->is_dir ? 0755 : 0644;
		st->uid = st->gid = 0;
	}
	return rc;
}

/* rwx bits of 'st' that apply to the current user. */
static int perm_bits(const struct vfs_stat *st)
{
	if (cred_uid() == st->uid)
		return (st->mode >> 6) & 7;
	if (cred_gid() == st->gid)
		return (st->mode >> 3) & 7;
	return st->mode & 7;
}

int vfs_access(const char *path, int want)
{
	struct vfs_stat st;
	int rc;

	if (!cred_uid())
		return FS_OK;
	rc = vfs_stat(path, &st);
	if (rc < 0)
		return rc;
	return (perm_bits(&st) & want) == want ? FS_OK : FS_EACCES;
}

/* Write permission on the directory that holds 'path' (needed to create, delete or rename entries). */
static int access_parent(const char *path, int want)
{
	char full[VFS_PATH_MAX];
	char *slash;

	if (!cred_uid())
		return FS_OK;
	if (vfs_normalize(vfs_getcwd(), path, full, sizeof full))
		return FS_EINVAL;
	slash = full + kstrlen(full);
	while (slash > full && *slash != '/')
		slash--;
	if (slash == full)
		return vfs_access("/", want);
	*slash = '\0';
	return vfs_access(full, want);
}

int vfs_chmod(const char *path, uint16_t mode)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);
	struct vfs_stat st;
	int rc = vfs_stat(path, &st);

	if (rc < 0)
		return rc;
	if (cred_uid() && cred_uid() != st.uid)
		return FS_EACCES;
	return m && m->ops->chmod ? m->ops->chmod(m->ctx, rest, mode) : FS_EROFS;
}

int vfs_chown(const char *path, uint16_t uid, uint16_t gid)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	if (cred_uid())
		return FS_EACCES;
	return m && m->ops->chown ? m->ops->chown(m->ctx, rest, uid, gid) : FS_EROFS;
}

int vfs_touch(const char *path, uint32_t mtime)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);
	int rc = vfs_access(path, VFS_W);

	if (rc)
		return rc;
	return m && m->ops->touch ? m->ops->touch(m->ctx, rest, mtime) : FS_EROFS;
}

int vfs_lstat(const char *path, struct vfs_stat *st)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup_nofollow(path, full, sizeof full, &rest);

	if (!m || !m->ops->stat)
		return FS_ENOENT;
	memset(st, 0, sizeof *st);
	return m->ops->stat(m->ctx, rest, st);
}

int vfs_readlink(const char *path, char *buf, uint32_t cap)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup_nofollow(path, full, sizeof full, &rest);

	if (!m || !m->ops->readlink)
		return FS_EINVAL;
	return m->ops->readlink(m->ctx, rest, buf, cap);
}

int vfs_link(const char *existing, const char *path)
{
	char f1[VFS_PATH_MAX], f2[VFS_PATH_MAX];
	const char *r1, *r2;
	struct mount *m1 = lookup(existing, f1, sizeof f1, &r1);
	struct mount *m2 = lookup_nofollow(path, f2, sizeof f2, &r2);
	int rc = vfs_access(existing, VFS_R) ? FS_EACCES : access_parent(path, VFS_W);

	if (!m1 || m1 != m2)
		return FS_EINVAL; /* names must be on the same filesystem */
	if (rc)
		return rc;
	return m1->ops->link ? m1->ops->link(m1->ctx, r1, r2) : FS_EROFS;
}

int vfs_symlink(const char *target, const char *linkpath)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup_nofollow(linkpath, full, sizeof full, &rest);
	int rc = access_parent(linkpath, VFS_W);

	if (!m)
		return FS_ENOENT;
	if (rc)
		return rc;
	return m->ops->symlink ? m->ops->symlink(m->ctx, target, rest) : FS_EROFS;
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
	int rc = vfs_access(path, VFS_R);

	if (rc)
		return rc;
	return m && m->ops->read ? m->ops->read(m->ctx, rest, buf, cap) : FS_ENOENT;
}

int vfs_write(const char *path, const void *data, uint32_t size)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	struct vfs_stat st;
	int rc;

	if (!m)
		return FS_ENOENT;
	rc = vfs_stat(path, &st) >= 0 ? vfs_access(path, VFS_W) : access_parent(path, VFS_W);
	if (rc)
		return rc;
	pcache_invalidate(full);
	return m->ops->write ? m->ops->write(m->ctx, rest, data, size) : FS_EROFS;
}

int vfs_create(const char *path)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	int rc = access_parent(path, VFS_W);

	if (!m)
		return FS_ENOENT;
	if (rc)
		return rc;
	return m->ops->create ? m->ops->create(m->ctx, rest) : FS_EROFS;
}

int vfs_unlink(const char *path)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup_nofollow(path, full, sizeof full, &rest);

	int rc = access_parent(path, VFS_W);

	if (!m)
		return FS_ENOENT;
	if (rc)
		return rc;
	pcache_invalidate(full);
	return m->ops->unlink ? m->ops->unlink(m->ctx, rest) : FS_EROFS;
}

int vfs_rename(const char *from, const char *to)
{
	char f1[VFS_PATH_MAX], f2[VFS_PATH_MAX];
	const char *r1, *r2;
	struct mount *m1 = lookup_nofollow(from, f1, sizeof f1, &r1);
	struct mount *m2 = lookup_nofollow(to, f2, sizeof f2, &r2);

	int rc = access_parent(from, VFS_W) ? FS_EACCES : access_parent(to, VFS_W);

	if (!m1 || m1 != m2)
		return FS_EINVAL; /* across mounts: not supported */
	if (rc)
		return rc;
	pcache_invalidate(f1);
	pcache_invalidate(f2);
	if (!m1->ops->rename)
		return FS_EROFS;
	return m1->ops->rename(m1->ctx, r1, r2);
}

/* Can files under this path be created or changed at all? (False on read-only mounts.) */
int vfs_can_write(const char *path)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	return m && m->ops->write != 0;
}

int vfs_mkdir(const char *path)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	int rc = access_parent(path, VFS_W);

	if (!m)
		return FS_ENOENT;
	if (rc)
		return rc;
	return m->ops->mkdir ? m->ops->mkdir(m->ctx, rest) : FS_EROFS;
}

int vfs_rmdir(const char *path)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	int rc = access_parent(path, VFS_W);

	if (!m)
		return FS_ENOENT;
	if (rc)
		return rc;
	return m->ops->rmdir ? m->ops->rmdir(m->ctx, rest) : FS_EROFS;
}

int vfs_list(const char *path, struct vfs_dirent *out, int max)
{
	char full[VFS_PATH_MAX];
	const char *rest;
	struct mount *m = lookup(path, full, sizeof full, &rest);

	int rc = vfs_access(path, VFS_R), n, i;

	if (rc)
		return rc;
	if (!m || !m->ops->list)
		return FS_ENOENT;
	memset(out, 0, (size_t)max * sizeof *out);
	n = m->ops->list(m->ctx, rest, out, max);
	for (i = 0; i < n; i++) {
		if (!out[i].has_meta) {
			out[i].mode = out[i].is_dir ? 0755 : 0644;
			out[i].uid = out[i].gid = 0;
		}
	}
	return n;
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
