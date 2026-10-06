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

struct vfs_dirent {
	char name[VFS_NAME_MAX];
	uint32_t size;
	int is_dir;
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
};

struct vfs_ops {
	const char *name;
	int (*stat)(void *ctx, const char *path, struct vfs_stat *st);
	int (*read)(void *ctx, const char *path, void *buf, uint32_t cap);   /* bytes read */
	int (*write)(void *ctx, const char *path, const void *data, uint32_t size); /* create/replace */
	int (*create)(void *ctx, const char *path);
	int (*unlink)(void *ctx, const char *path);
	int (*list)(void *ctx, const char *path, struct vfs_dirent *out, int max);  /* entries */
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
int  vfs_list(const char *path, struct vfs_dirent *out, int max);
void vfs_print_mounts(void);
int  vfs_format_mounts(char *buf, uint32_t cap);   /* text for /proc/mounts */

#endif
