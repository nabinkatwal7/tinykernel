#ifndef FS_H
#define FS_H

#include <stdint.h>

/*
 * TinyFS v2: contiguous-allocation files in a hierarchy of directories.
 *   sector 0         superblock (magic, version, disk size)
 *   sectors 1-8      entry table: 128 fixed entries of 32 bytes (files and directories)
 *   sectors 9..      file data; every file occupies one contiguous extent
 * An entry names its parent directory by table index (0xFF = root), so a directory is just an
 * entry with the DIR flag and no data. Paths use '/' separators; "a/b" and "/a/b" are the same.
 */
#define FS_NAME_MAX  20 /* one path component, including the NUL */
#define FS_MAX_FILES 128 /* entries in total (files + directories) */

#define FS_OK        0
#define FS_ENOENT   -1 /* no such file or directory */
#define FS_EEXIST   -2
#define FS_ENOSPC   -3 /* disk or entry table full */
#define FS_EIO      -4
#define FS_EINVAL   -5 /* bad name or argument */
#define FS_ENOMOUNT -6 /* no disk, or disk not formatted */
#define FS_ETOOBIG  -7 /* destination buffer too small */
#define FS_EISDIR   -8 /* is a directory (file operation on a directory) */
#define FS_ENOTDIR  -9 /* a path component is not a directory */
#define FS_ENOTEMPTY -10 /* rmdir on a directory that still has entries */
#define FS_EROFS    -11 /* read-only filesystem */
#define FS_EPIPE    -12 /* write to a pipe with no reader */
#define FS_EACCES   -13 /* permission denied */
#define FS_EBUSY    -14 /* locked by someone else (non-blocking request) */

/* Per-entry metadata (TinyFS keeps one 16-byte record per directory entry). */
struct fs_meta {
	uint16_t mode;          /* permission bits, 0777 style: owner, group, other; each r=4 w=2 x=1 */
	uint16_t uid, gid;
	uint16_t flags;         /* reserved */
	uint32_t mtime, ctime;  /* seconds since 1970: last write, creation */
};

struct fs_stat {
	char name[FS_NAME_MAX];
	uint32_t size;
	uint32_t start_lba;
	uint32_t sectors;
	int is_dir;
	int is_link;            /* a symbolic link (its contents are the target path) */
	int nlink;              /* names for this file (hard links) */
	struct fs_meta meta;
};

int         fs_mount(void);              /* reads the table; FS_ENOMOUNT if unusable */
int         fs_mounted(void);
int         fs_format(void);
int         fs_create(const char *path);
int         fs_write(const char *path, const void *data, uint32_t size); /* create or replace */
int         fs_read(const char *path, void *buf, uint32_t cap);          /* bytes read or error */
int         fs_size(const char *path);   /* bytes (FS_EISDIR for a directory) or error */
int         fs_stat(const char *path, struct fs_stat *st);
int         fs_link(const char *existing, const char *path); /* a second name for a (non-empty) file */
int         fs_symlink(const char *target, const char *path); /* create a symbolic link at path */
int         fs_readlink(const char *path, char *buf, uint32_t cap); /* target length, or FS_EINVAL if path is not a link */
int         fs_chmod(const char *path, uint16_t mode);
int         fs_chown(const char *path, uint16_t uid, uint16_t gid);
int         fs_touch(const char *path, uint32_t mtime); /* set the modification time */
int         fs_delete(const char *path); /* files only */
int         fs_mkdir(const char *path);
int         fs_rmdir(const char *path);  /* must be empty */
/* Move/rename a file or directory. An existing *file* at the destination is replaced; moving a
   directory into itself is refused (FS_EINVAL). */
int         fs_rename(const char *from, const char *to);
int         fs_list(const char *dir, struct fs_stat *out, int max); /* entries in a directory */
uint32_t    fs_free_sectors(void);

/*
 * Consistency check. Looks at every entry (valid name, parent exists and is a directory, no
 * cycles, no duplicate names), every file extent (inside the data area, no two files share a
 * sector) and the free-space bitmap (agrees with the extents). With repair != 0 it deletes
 * unusable entries and rebuilds the bitmap. 'report' is called once per finding.
 */
struct fs_check_result {
	int problems;     /* findings */
	int repaired;     /* findings that were fixed (repair mode) */
};
int         fs_check(int repair, void (*report)(const char *msg), struct fs_check_result *res);
/* Test hook: damage the filesystem on purpose (0 = leak a sector, 1 = free a used sector,
   2 = orphan an entry, 3 = overlap two files). Returns 0 if the damage could be applied. */
int         fs_debug_corrupt(int kind);

struct fs_info {
	uint32_t total_sectors;   /* whole disk */
	uint32_t used_sectors;    /* metadata + file data */
	uint32_t free_sectors;
	uint32_t largest_free;    /* longest contiguous run: the biggest file that can still be written */
	uint32_t files, dirs;
	uint32_t free_entries;    /* unused slots in the entry table */
};
int         fs_info(struct fs_info *out);   /* FS_ENOMOUNT if nothing is mounted */
const char *fs_strerror(int err);

#endif
