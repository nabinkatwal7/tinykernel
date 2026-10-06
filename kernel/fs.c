#include "fs.h"

#include "ata.h"
#include "bcache.h"
#include "klog.h"
#include "kmalloc.h"
#include "kstring.h"

#define FS_MAGIC     0x31534654u /* "TFS1" */
#define FS_VERSION   3
#define DIR_SECTORS  8     /* 8 * 512 / 32 = 128 entries */
#define FLAG_USED    1u
#define FLAG_DIR     2u
#define ROOT         0xFFu /* parent value meaning "the root directory" */

/* Disk layout, fixed at format time and stored in the superblock:
 *   0 superblock | bm_lba.. free-space bitmap (1 bit per sector) | dir_lba.. entry table | data */
static uint32_t total_sectors, bm_lba, bm_sectors, dir_lba, data_start;
static uint8_t *bitmap;   /* bit set = sector in use */

struct dirent {
	char name[FS_NAME_MAX];
	uint32_t start;
	uint32_t size;
	uint32_t flags; /* bit 0 used, bit 1 directory, bits 8-15 parent entry index */
};

static struct dirent dir[FS_MAX_FILES]; /* exactly DIR_SECTORS sectors */
static int mounted;

const char *fs_strerror(int err)
{
	switch (err) {
	case FS_OK:        return "ok";
	case FS_ENOENT:    return "no such file or directory";
	case FS_EEXIST:    return "already exists";
	case FS_ENOSPC:    return "no space left";
	case FS_EIO:       return "disk I/O error";
	case FS_EINVAL:    return "invalid name or argument";
	case FS_ENOMOUNT:  return "no formatted disk (try 'format')";
	case FS_ETOOBIG:   return "buffer too small";
	case FS_EISDIR:    return "is a directory";
	case FS_ENOTDIR:   return "not a directory";
	case FS_ENOTEMPTY: return "directory not empty";
	}
	return "unknown error";
}

static uint32_t sectors_for(uint32_t size)
{
	return (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
}

static int used(int i)      { return dir[i].flags & FLAG_USED; }
static int is_dir(int i)    { return dir[i].flags & FLAG_DIR; }
static uint32_t parent(int i) { return (dir[i].flags >> 8) & 0xFF; }

static int flush_dir(void)
{
	return bc_write(dir_lba, DIR_SECTORS, dir) ? FS_EIO : FS_OK;
}

static int flush_bitmap(void)
{
	return bc_write(bm_lba, bm_sectors, bitmap) ? FS_EIO : FS_OK;
}

static int bit_get(uint32_t sec)
{
	return bitmap[sec >> 3] & (1u << (sec & 7));
}

static void mark(uint32_t start, uint32_t n, int used_flag)
{
	uint32_t i;

	for (i = start; i < start + n; i++) {
		if (used_flag)
			bitmap[i >> 3] |= (uint8_t)(1u << (i & 7));
		else
			bitmap[i >> 3] &= (uint8_t)~(1u << (i & 7));
	}
}

/* Mark the data sectors of entry i free/used (directories own none). */
static void mark_entry(int i, int used_flag)
{
	if (!is_dir(i) && dir[i].size)
		mark(dir[i].start, sectors_for(dir[i].size), used_flag);
}

static int name_ok(const char *name)
{
	size_t n = kstrlen(name), i;

	if (n == 0 || n >= FS_NAME_MAX)
		return 0;
	if (!kstrcmp(name, ".") || !kstrcmp(name, ".."))
		return 0;
	for (i = 0; i < n; i++)
		if (name[i] <= ' ' || name[i] > '~' || name[i] == '/')
			return 0;
	return 1;
}

/* Entry called 'name' inside directory 'par' (an entry index or ROOT), or -1. */
static int find_in(uint32_t par, const char *name)
{
	int i;

	for (i = 0; i < FS_MAX_FILES; i++)
		if (used(i) && parent(i) == par && !kstrcmp(dir[i].name, name))
			return i;
	return -1;
}

/*
 * Walk a path. On success *leaf points at the final component (may be "" for the root) and the
 * return value is the index of the directory that should contain it (ROOT for top level).
 * Negative: FS_ENOENT if an intermediate directory is missing, FS_ENOTDIR if it is a file.
 */
static int walk_parent(const char *path, char *leaf)
{
	uint32_t cur = ROOT;
	char comp[FS_NAME_MAX];

	for (;;) {
		size_t n = 0;

		while (*path == '/')
			path++;
		while (path[n] && path[n] != '/')
			n++;
		if (n >= FS_NAME_MAX)
			return FS_EINVAL;
		memcpy(comp, path, n);
		comp[n] = '\0';
		path += n;
		while (*path == '/')
			path++;
		if (!*path) { /* that was the last component */
			kstrlcpy(leaf, comp, FS_NAME_MAX);
			return (int)cur;
		}
		{
			int i = find_in(cur, comp);

			if (i < 0)
				return FS_ENOENT;
			if (!is_dir(i))
				return FS_ENOTDIR;
			cur = (uint32_t)i;
		}
	}
}

/* Entry index for a path: ROOT for "/" itself, else the entry, or a negative error. */
static int resolve(const char *path)
{
	char leaf[FS_NAME_MAX];
	int par = walk_parent(path, leaf);
	int i;

	if (par < 0)
		return par;
	if (!leaf[0])
		return (int)ROOT; /* "" or "/" */
	i = find_in((uint32_t)par, leaf);
	return i < 0 ? FS_ENOENT : i;
}

/* First-fit over the bitmap: n free contiguous sectors, or 0 if there is no such run. */
static uint32_t alloc_extent(uint32_t n)
{
	uint32_t run = 0, start = 0, sec;

	if (n == 0)
		return data_start;
	for (sec = data_start; sec < total_sectors; sec++) {
		if (bit_get(sec)) {
			run = 0;
			continue;
		}
		if (run++ == 0)
			start = sec;
		if (run == n)
			return start;
	}
	return 0;
}

static int free_slot(void)
{
	int i;

	for (i = 0; i < FS_MAX_FILES; i++)
		if (!used(i))
			return i;
	return -1;
}

int fs_mounted(void)
{
	return mounted;
}

int fs_mount(void)
{
	uint32_t sb[SECTOR_SIZE / 4];

	mounted = 0;
	kfree(bitmap);
	bitmap = 0;
	if (!ata_present())
		return FS_ENOMOUNT;
	if (bc_read(0, 1, sb))
		return FS_EIO;
	if (sb[0] != FS_MAGIC || sb[1] != FS_VERSION)
		return FS_ENOMOUNT;
	total_sectors = sb[2];
	bm_lba = sb[3];
	bm_sectors = sb[4];
	dir_lba = sb[5];
	data_start = sb[6];
	if (total_sectors != ata_sectors() || bm_sectors == 0 || data_start >= total_sectors)
		return FS_ENOMOUNT;
	bitmap = kmalloc(bm_sectors * SECTOR_SIZE);
	if (!bitmap)
		return FS_ENOSPC;
	if (bc_read(bm_lba, bm_sectors, bitmap) || bc_read(dir_lba, DIR_SECTORS, dir))
		return FS_EIO;
	mounted = 1;
	return FS_OK;
}

int fs_format(void)
{
	uint32_t sb[SECTOR_SIZE / 4];

	if (!ata_present())
		return FS_ENOMOUNT;
	kfree(bitmap);
	total_sectors = ata_sectors();
	bm_lba = 1;
	bm_sectors = (total_sectors + SECTOR_SIZE * 8 - 1) / (SECTOR_SIZE * 8);
	dir_lba = bm_lba + bm_sectors;
	data_start = dir_lba + DIR_SECTORS;
	if (data_start >= total_sectors)
		return FS_ENOSPC; /* disk too small to hold the metadata */
	bitmap = kcalloc(bm_sectors, SECTOR_SIZE);
	if (!bitmap)
		return FS_ENOSPC;
	mark(0, data_start, 1); /* superblock, bitmap and entry table are never allocatable */

	memset(sb, 0, sizeof sb);
	sb[0] = FS_MAGIC;
	sb[1] = FS_VERSION;
	sb[2] = total_sectors;
	sb[3] = bm_lba;
	sb[4] = bm_sectors;
	sb[5] = dir_lba;
	sb[6] = data_start;
	memset(dir, 0, sizeof dir);
	if (bc_write(0, 1, sb) || flush_bitmap() || flush_dir())
		return FS_EIO;
	mounted = 1;
	klog(LOG_INFO, "fs: formatted %u sectors (bitmap %u, data from %u)", total_sectors, bm_sectors,
	     data_start);
	return FS_OK;
}

int fs_create(const char *path)
{
	if (!mounted)
		return FS_ENOMOUNT;
	if (resolve(path) >= 0)
		return FS_EEXIST;
	return fs_write(path, 0, 0);
}

int fs_write(const char *path, const void *data, uint32_t size)
{
	uint8_t bounce[SECTOR_SIZE];
	const uint8_t *src = data;
	char leaf[FS_NAME_MAX];
	struct dirent saved;
	uint32_t n = sectors_for(size), start, i;
	int par, idx;

	if (!mounted)
		return FS_ENOMOUNT;
	par = walk_parent(path, leaf);
	if (par < 0)
		return par;
	if (!name_ok(leaf))
		return FS_EINVAL;

	idx = find_in((uint32_t)par, leaf);
	if (idx >= 0) {
		if (is_dir(idx))
			return FS_EISDIR;
		saved = dir[idx];
		mark_entry(idx, 0); /* free the old extent while we look for space */
		dir[idx].flags = 0;
	} else {
		idx = free_slot();
		if (idx < 0)
			return FS_ENOSPC;
		memset(&saved, 0, sizeof saved);
	}

	start = alloc_extent(n);
	if (!start) {
		dir[idx] = saved;
		if (saved.flags)
			mark_entry(idx, 1);
		return FS_ENOSPC;
	}

	for (i = 0; i < n; i++) {
		uint32_t left = size - i * SECTOR_SIZE;

		if (left >= SECTOR_SIZE) {
			if (bc_write(start + i, 1, src + i * SECTOR_SIZE))
				goto io_error;
		} else {
			memset(bounce, 0, sizeof bounce);
			memcpy(bounce, src + i * SECTOR_SIZE, left);
			if (bc_write(start + i, 1, bounce))
				goto io_error;
		}
	}

	memset(&dir[idx], 0, sizeof dir[idx]);
	kstrlcpy(dir[idx].name, leaf, FS_NAME_MAX);
	dir[idx].start = start;
	dir[idx].size = size;
	dir[idx].flags = FLAG_USED | ((uint32_t)par << 8);
	mark(start, n, 1);
	if (flush_bitmap())
		return FS_EIO;
	return flush_dir();

io_error:
	dir[idx] = saved;
	if (saved.flags)
		mark_entry(idx, 1);
	return FS_EIO;
}

static int fs_entry(const char *path)
{
	int idx;

	if (!mounted)
		return FS_ENOMOUNT;
	idx = resolve(path);
	return idx;
}

int fs_size(const char *path)
{
	int idx = fs_entry(path);

	if (idx < 0)
		return idx;
	if (idx == (int)ROOT || is_dir(idx))
		return FS_EISDIR;
	return (int)dir[idx].size;
}

int fs_stat(const char *path, struct fs_stat *st)
{
	int idx = fs_entry(path);

	if (idx < 0)
		return idx;
	memset(st, 0, sizeof *st);
	if (idx == (int)ROOT) {
		st->is_dir = 1;
		return FS_OK;
	}
	kstrlcpy(st->name, dir[idx].name, FS_NAME_MAX);
	st->size = dir[idx].size;
	st->start_lba = dir[idx].start;
	st->sectors = sectors_for(dir[idx].size);
	st->is_dir = is_dir(idx) ? 1 : 0;
	return FS_OK;
}

int fs_read(const char *path, void *buf, uint32_t cap)
{
	uint8_t bounce[SECTOR_SIZE];
	uint8_t *dst = buf;
	uint32_t size, n, i;
	int idx = fs_entry(path);

	if (idx < 0)
		return idx;
	if (idx == (int)ROOT || is_dir(idx))
		return FS_EISDIR;
	size = dir[idx].size;
	if (size > cap)
		return FS_ETOOBIG;

	n = sectors_for(size);
	for (i = 0; i < n; i++) {
		uint32_t left = size - i * SECTOR_SIZE;

		if (left >= SECTOR_SIZE) {
			if (bc_read(dir[idx].start + i, 1, dst + i * SECTOR_SIZE))
				return FS_EIO;
		} else {
			if (bc_read(dir[idx].start + i, 1, bounce))
				return FS_EIO;
			memcpy(dst + i * SECTOR_SIZE, bounce, left);
		}
	}
	return (int)size;
}

int fs_delete(const char *path)
{
	int idx = fs_entry(path);

	if (idx < 0)
		return idx;
	if (idx == (int)ROOT || is_dir(idx))
		return FS_EISDIR;
	mark_entry(idx, 0);
	memset(&dir[idx], 0, sizeof dir[idx]);
	if (flush_bitmap())
		return FS_EIO;
	return flush_dir();
}

int fs_mkdir(const char *path)
{
	char leaf[FS_NAME_MAX];
	int par, idx;

	if (!mounted)
		return FS_ENOMOUNT;
	par = walk_parent(path, leaf);
	if (par < 0)
		return par;
	if (!name_ok(leaf))
		return FS_EINVAL;
	if (find_in((uint32_t)par, leaf) >= 0)
		return FS_EEXIST;
	idx = free_slot();
	if (idx < 0)
		return FS_ENOSPC;
	memset(&dir[idx], 0, sizeof dir[idx]);
	kstrlcpy(dir[idx].name, leaf, FS_NAME_MAX);
	dir[idx].flags = FLAG_USED | FLAG_DIR | ((uint32_t)par << 8);
	return flush_dir();
}

int fs_rmdir(const char *path)
{
	int idx = fs_entry(path), i;

	if (idx < 0)
		return idx;
	if (idx == (int)ROOT)
		return FS_EINVAL; /* cannot remove the root */
	if (!is_dir(idx))
		return FS_ENOTDIR;
	for (i = 0; i < FS_MAX_FILES; i++)
		if (used(i) && parent(i) == (uint32_t)idx)
			return FS_ENOTEMPTY;
	memset(&dir[idx], 0, sizeof dir[idx]);
	return flush_dir();
}

int fs_rename(const char *from, const char *to)
{
	char leaf[FS_NAME_MAX];
	int src = fs_entry(from), par, victim, up;

	if (src < 0)
		return src;
	if (src == (int)ROOT)
		return FS_EINVAL;
	par = walk_parent(to, leaf);
	if (par < 0)
		return par;
	if (!name_ok(leaf))
		return FS_EINVAL;
	/* a directory may not move into itself or below itself */
	for (up = par; up != (int)ROOT; up = (int)parent(up))
		if (up == src)
			return FS_EINVAL;
	victim = find_in((uint32_t)par, leaf);
	if (victim == src)
		return FS_OK; /* same place, same name */
	if (victim >= 0) {
		if (is_dir(victim) || is_dir(src))
			return FS_EEXIST;
		mark_entry(victim, 0);
		memset(&dir[victim], 0, sizeof dir[victim]); /* replace the old file */
		if (flush_bitmap())
			return FS_EIO;
	}
	kstrlcpy(dir[src].name, leaf, FS_NAME_MAX);
	dir[src].flags = (dir[src].flags & ~(0xFFu << 8)) | ((uint32_t)par << 8);
	return flush_dir();
}

int fs_list(const char *path, struct fs_stat *out, int max)
{
	int idx = fs_entry(path), i, n = 0;

	if (idx < 0)
		return idx;
	if (idx != (int)ROOT && !is_dir(idx))
		return FS_ENOTDIR;
	for (i = 0; i < FS_MAX_FILES && n < max; i++) {
		if (!used(i) || parent(i) != (uint32_t)idx)
			continue;
		kstrlcpy(out[n].name, dir[i].name, FS_NAME_MAX);
		out[n].size = dir[i].size;
		out[n].start_lba = dir[i].start;
		out[n].sectors = sectors_for(dir[i].size);
		out[n].is_dir = is_dir(i) ? 1 : 0;
		n++;
	}
	return n;
}

uint32_t fs_free_sectors(void)
{
	uint32_t free_count = 0, sec;

	if (!mounted)
		return 0;
	for (sec = 0; sec < total_sectors; sec++)
		free_count += !bit_get(sec);
	return free_count;
}

int fs_info(struct fs_info *out)
{
	uint32_t sec, run = 0;
	int i;

	if (!mounted)
		return FS_ENOMOUNT;
	memset(out, 0, sizeof *out);
	out->total_sectors = total_sectors;
	for (i = 0; i < FS_MAX_FILES; i++) {
		if (!used(i))
			out->free_entries++;
		else if (is_dir(i))
			out->dirs++;
		else
			out->files++;
	}
	for (sec = 0; sec < total_sectors; sec++) {
		if (bit_get(sec)) {
			out->used_sectors++;
			run = 0;
		} else {
			out->free_sectors++;
			if (++run > out->largest_free)
				out->largest_free = run;
		}
	}
	return FS_OK;
}
