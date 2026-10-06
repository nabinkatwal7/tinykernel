#include "fs.h"

#include "ata.h"
#include "klog.h"
#include "kstring.h"

#define FS_MAGIC     0x31534654u /* "TFS1" */
#define DIR_LBA      1
#define DIR_SECTORS  2
#define DATA_START   3
#define FLAG_USED    1u

struct dirent {
	char name[FS_NAME_MAX];
	uint32_t start;
	uint32_t size;
	uint32_t flags;
};

static struct dirent dir[FS_MAX_FILES]; /* 32 * 32 bytes == DIR_SECTORS sectors */
static int mounted;

const char *fs_strerror(int err)
{
	switch (err) {
	case FS_OK:       return "ok";
	case FS_ENOENT:   return "no such file";
	case FS_EEXIST:   return "file exists";
	case FS_ENOSPC:   return "no space left";
	case FS_EIO:      return "disk I/O error";
	case FS_EINVAL:   return "invalid name or argument";
	case FS_ENOMOUNT: return "no formatted disk (try 'format')";
	case FS_ETOOBIG:  return "buffer too small";
	}
	return "unknown error";
}

static uint32_t sectors_for(uint32_t size)
{
	return (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
}

static int flush_dir(void)
{
	return ata_write(DIR_LBA, DIR_SECTORS, dir) ? FS_EIO : FS_OK;
}

static int name_ok(const char *name)
{
	size_t n = kstrlen(name), i;

	if (n == 0 || n >= FS_NAME_MAX)
		return 0;
	for (i = 0; i < n; i++)
		if (name[i] <= ' ' || name[i] > '~' || name[i] == '/')
			return 0;
	return 1;
}

static int find(const char *name)
{
	int i;

	for (i = 0; i < FS_MAX_FILES; i++)
		if ((dir[i].flags & FLAG_USED) && !kstrcmp(dir[i].name, name))
			return i;
	return -1;
}

/* First-fit: find n free contiguous sectors, or 0 if the disk is too full/fragmented. */
static uint32_t alloc_extent(uint32_t n)
{
	uint32_t cand = DATA_START, total = ata_sectors();
	int moved, i;

	if (n == 0)
		return DATA_START;
	do {
		moved = 0;
		for (i = 0; i < FS_MAX_FILES; i++) {
			uint32_t s, e;

			if (!(dir[i].flags & FLAG_USED))
				continue;
			s = dir[i].start;
			e = s + sectors_for(dir[i].size);
			if (cand < e && cand + n > s) { /* overlaps this file: jump past it */
				cand = e;
				moved = 1;
			}
		}
	} while (moved);
	return cand + n <= total ? cand : 0;
}

int fs_mounted(void)
{
	return mounted;
}

int fs_mount(void)
{
	uint32_t sb[SECTOR_SIZE / 4];

	mounted = 0;
	if (!ata_present())
		return FS_ENOMOUNT;
	if (ata_read(0, 1, sb))
		return FS_EIO;
	if (sb[0] != FS_MAGIC)
		return FS_ENOMOUNT;
	if (ata_read(DIR_LBA, DIR_SECTORS, dir))
		return FS_EIO;
	mounted = 1;
	return FS_OK;
}

int fs_format(void)
{
	uint32_t sb[SECTOR_SIZE / 4];

	if (!ata_present())
		return FS_ENOMOUNT;
	memset(sb, 0, sizeof sb);
	sb[0] = FS_MAGIC;
	sb[1] = 1; /* version */
	sb[2] = ata_sectors();
	memset(dir, 0, sizeof dir);
	if (ata_write(0, 1, sb))
		return FS_EIO;
	if (flush_dir())
		return FS_EIO;
	mounted = 1;
	klog(LOG_INFO, "fs: formatted %u sectors", ata_sectors());
	return FS_OK;
}

int fs_create(const char *name)
{
	if (mounted && find(name) >= 0)
		return FS_EEXIST;
	return fs_write(name, 0, 0);
}

int fs_write(const char *name, const void *data, uint32_t size)
{
	uint8_t bounce[SECTOR_SIZE];
	const uint8_t *src = data;
	struct dirent saved;
	uint32_t n = sectors_for(size), start, i;
	int idx;

	if (!mounted)
		return FS_ENOMOUNT;
	if (!name_ok(name))
		return FS_EINVAL;

	idx = find(name);
	if (idx >= 0) {
		saved = dir[idx];
		dir[idx].flags = 0; /* free the old extent while we look for space */
	} else {
		for (idx = 0; idx < FS_MAX_FILES && (dir[idx].flags & FLAG_USED); idx++)
			;
		if (idx == FS_MAX_FILES)
			return FS_ENOSPC;
		memset(&saved, 0, sizeof saved);
	}

	start = alloc_extent(n);
	if (!start) {
		dir[idx] = saved;
		return FS_ENOSPC;
	}

	for (i = 0; i < n; i++) {
		uint32_t left = size - i * SECTOR_SIZE;

		if (left >= SECTOR_SIZE) {
			if (ata_write(start + i, 1, src + i * SECTOR_SIZE))
				goto io_error;
		} else {
			memset(bounce, 0, sizeof bounce);
			memcpy(bounce, src + i * SECTOR_SIZE, left);
			if (ata_write(start + i, 1, bounce))
				goto io_error;
		}
	}

	memset(&dir[idx], 0, sizeof dir[idx]);
	kstrlcpy(dir[idx].name, name, FS_NAME_MAX);
	dir[idx].start = start;
	dir[idx].size = size;
	dir[idx].flags = FLAG_USED;
	return flush_dir();

io_error:
	dir[idx] = saved;
	return FS_EIO;
}

int fs_size(const char *name)
{
	int idx;

	if (!mounted)
		return FS_ENOMOUNT;
	idx = find(name);
	return idx < 0 ? FS_ENOENT : (int)dir[idx].size;
}

int fs_read(const char *name, void *buf, uint32_t cap)
{
	uint8_t bounce[SECTOR_SIZE];
	uint8_t *dst = buf;
	uint32_t size, n, i;
	int idx;

	if (!mounted)
		return FS_ENOMOUNT;
	idx = find(name);
	if (idx < 0)
		return FS_ENOENT;
	size = dir[idx].size;
	if (size > cap)
		return FS_ETOOBIG;

	n = sectors_for(size);
	for (i = 0; i < n; i++) {
		uint32_t left = size - i * SECTOR_SIZE;

		if (left >= SECTOR_SIZE) {
			if (ata_read(dir[idx].start + i, 1, dst + i * SECTOR_SIZE))
				return FS_EIO;
		} else {
			if (ata_read(dir[idx].start + i, 1, bounce))
				return FS_EIO;
			memcpy(dst + i * SECTOR_SIZE, bounce, left);
		}
	}
	return (int)size;
}

int fs_delete(const char *name)
{
	int idx;

	if (!mounted)
		return FS_ENOMOUNT;
	idx = find(name);
	if (idx < 0)
		return FS_ENOENT;
	memset(&dir[idx], 0, sizeof dir[idx]);
	return flush_dir();
}

int fs_list(struct fs_stat *out, int max)
{
	int i, n = 0;

	if (!mounted)
		return FS_ENOMOUNT;
	for (i = 0; i < FS_MAX_FILES && n < max; i++) {
		if (!(dir[i].flags & FLAG_USED))
			continue;
		kstrlcpy(out[n].name, dir[i].name, FS_NAME_MAX);
		out[n].size = dir[i].size;
		out[n].start_lba = dir[i].start;
		out[n].sectors = sectors_for(dir[i].size);
		n++;
	}
	return n;
}

uint32_t fs_free_sectors(void)
{
	uint32_t used = DATA_START;
	int i;

	for (i = 0; i < FS_MAX_FILES; i++)
		if (dir[i].flags & FLAG_USED)
			used += sectors_for(dir[i].size);
	return ata_sectors() > used ? ata_sectors() - used : 0;
}
