#ifndef FS_H
#define FS_H

#include <stdint.h>

/*
 * TinyFS: a flat, contiguous-allocation filesystem.
 *   sector 0        superblock (magic, version)
 *   sectors 1-2     directory: 32 fixed entries of 32 bytes
 *   sectors 3..     file data; every file occupies one contiguous extent
 */
#define FS_NAME_MAX  20 /* including the NUL */
#define FS_MAX_FILES 32

#define FS_OK        0
#define FS_ENOENT   -1 /* no such file */
#define FS_EEXIST   -2
#define FS_ENOSPC   -3 /* disk or directory full */
#define FS_EIO      -4
#define FS_EINVAL   -5 /* bad name or argument */
#define FS_ENOMOUNT -6 /* no disk, or disk not formatted */
#define FS_ETOOBIG  -7 /* destination buffer too small */

struct fs_stat {
	char name[FS_NAME_MAX];
	uint32_t size;
	uint32_t start_lba;
	uint32_t sectors;
};

int         fs_mount(void);              /* reads the directory; FS_ENOMOUNT if unusable */
int         fs_mounted(void);
int         fs_format(void);
int         fs_create(const char *name);
int         fs_write(const char *name, const void *data, uint32_t size); /* create or replace */
int         fs_read(const char *name, void *buf, uint32_t cap);          /* bytes read or error */
int         fs_size(const char *name);   /* bytes or error */
int         fs_delete(const char *name);
int         fs_list(struct fs_stat *out, int max); /* number of files */
uint32_t    fs_free_sectors(void);
const char *fs_strerror(int err);

#endif
