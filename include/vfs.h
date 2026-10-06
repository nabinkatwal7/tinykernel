#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#include "fs.h"

/*
 * Virtual filesystem layer. A filesystem driver supplies a table of whole-file operations; the
 * VFS keeps a mount table, picks the driver for a path by longest matching mount prefix and hands
 * it the remainder of the path. Errors are the FS_E* codes from fs.h (negative).
 */
#define VFS_NAME_MAX 24
#define VFS_PATH_MAX 64 /* longest absolute path, including the NUL */

struct vfs_dirent {
	char name[VFS_NAME_MAX];
	uint32_t size;
	int is_dir;
	int is_link;
	int has_meta;            /* the filesystem supplied owner/mode/time (else defaults are filled in) */
	uint16_t mode, uid, gid;
	uint32_t mtime;
};

/* A character device: a stream with no size, read/written in whatever amounts the caller asks. */
struct vfs_device {
	const char *name;
	int (*read)(void *buf, uint32_t n);        /* bytes produced (0 = end of input), or < 0 */
	int (*write)(const void *buf, uint32_t n); /* bytes consumed, or < 0 */
};

struct vfs_stat {
	uint32_t size;
	int is_dir;
	const struct vfs_device *dev;              /* non-NULL for device files */
	int has_meta;
	int is_link;                               /* (lstat only) this is a symbolic link */
	uint16_t mode, uid, gid;                   /* permissions (0777 style) and owner; defaults on filesystems without them */
	uint32_t mtime, ctime;                     /* last modification and creation, seconds since 1970 (0 = not recorded) */
};

struct vfs_ops {
	const char *name;
	int (*stat)(void *ctx, const char *path, struct vfs_stat *st);
	int (*read)(void *ctx, const char *path, void *buf, uint32_t cap);   /* bytes read */
	int (*write)(void *ctx, const char *path, const void *data, uint32_t size); /* create/replace */
	int (*create)(void *ctx, const char *path);
	int (*unlink)(void *ctx, const char *path);
	int (*list)(void *ctx, const char *path, struct vfs_dirent *out, int max);  /* entries */
	int (*mkdir)(void *ctx, const char *path);
	int (*rmdir)(void *ctx, const char *path);
	int (*rename)(void *ctx, const char *from, const char *to);
	int (*chmod)(void *ctx, const char *path, uint16_t mode);
	int (*chown)(void *ctx, const char *path, uint16_t uid, uint16_t gid);
	int (*touch)(void *ctx, const char *path, uint32_t mtime);
	int (*symlink)(void *ctx, const char *target, const char *path);
	int (*readlink)(void *ctx, const char *path, char *buf, uint32_t cap);
};

#define VFS_MAX_MOUNTS 8

int  vfs_mount(const char *prefix, const struct vfs_ops *ops, void *ctx); /* 0 or FS_E* */
int  vfs_unmount(const char *prefix);
void vfs_init(void);

/* devfs: mounted on /dev by vfs_init(). */
int  devfs_register(const struct vfs_device *dev);   /* FS_ENOSPC when the table is full */
void devfs_init(void);
void procfs_init(void);                              /* read-only status files on /proc */                               /* registers the built-in devices */                    /* mounts TinyFS on "/" */

int  vfs_stat(const char *path, struct vfs_stat *st);
int  vfs_size(const char *path);        /* bytes, or a negative error */
int  vfs_read(const char *path, void *buf, uint32_t cap);
int  vfs_write(const char *path, const void *data, uint32_t size);
int  vfs_create(const char *path);
int  vfs_unlink(const char *path);
int  vfs_can_write(const char *path);  /* 0 on a read-only mount */
int  vfs_mkdir(const char *path);
int  vfs_rmdir(const char *path);
int  vfs_rename(const char *from, const char *to); /* both paths must be on the same mount */
int  vfs_list(const char *path, struct vfs_dirent *out, int max);

/* Permissions. want is a mix of VFS_R, VFS_W, VFS_X; root passes everything. Returns 0 or FS_EACCES (or a lookup error). */
#define VFS_R 4
#define VFS_W 2
#define VFS_X 1
int  vfs_access(const char *path, int want);

/*
 * Symbolic links (TinyFS only). Every path is resolved through links - in the middle and at the end - before
 * it is used; unlink, rename, readlink and lstat act on the link itself.
 */
int  vfs_symlink(const char *target, const char *linkpath);
int  vfs_readlink(const char *path, char *buf, uint32_t cap);  /* target length, or FS_EINVAL if not a link */
int  vfs_lstat(const char *path, struct vfs_stat *st);         /* like vfs_stat, but does not follow a final link */
int  vfs_chmod(const char *path, uint16_t mode);   /* owner or root only */
int  vfs_chown(const char *path, uint16_t uid, uint16_t gid); /* root only */
int  vfs_touch(const char *path, uint32_t mtime);  /* 0 = now */
/* Canonical absolute form of 'path' (relative paths start at 'base'): duplicate slashes collapse,
   '.' disappears, '..' removes the previous component (the root is its own parent), no trailing
   slash. Returns 0, or FS_EINVAL if the result would not fit in 'size' bytes. */
int  vfs_normalize(const char *base, const char *path, char *out, uint32_t size);

const char *vfs_getcwd(void);          /* current working directory (absolute) */
int  vfs_chdir(const char *path);        /* 0, FS_ENOENT or FS_ENOTDIR */
void vfs_print_mounts(void);
int  vfs_format_mounts(char *buf, uint32_t cap);   /* text for /proc/mounts */

#endif
