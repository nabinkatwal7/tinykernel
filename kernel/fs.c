#include "fs.h"

#include "ata.h"
#include "klog.h"
#include "kstring.h"

#define FS_MAGIC     0x31534654u /* "TFS1" */
#define FS_VERSION   2
#define DIR_LBA      1
#define DIR_SECTORS  8     /* 8 * 512 / 32 = 128 entries */
#define DATA_START   9
#define FLAG_USED    1u
#define FLAG_DIR     2u
#define ROOT         0xFFu /* parent value meaning "the root directory" */

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
	return ata_write(DIR_LBA, DIR_SECTORS, dir) ? FS_EIO : FS_OK;
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

			if (!used(i) || is_dir(i))
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
	if (!ata_present())
		return FS_ENOMOUNT;
	if (ata_read(0, 1, sb))
		return FS_EIO;
	if (sb[0] != FS_MAGIC || sb[1] != FS_VERSION)
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
	sb[1] = FS_VERSION;
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
		dir[idx].flags = 0; /* free the old extent while we look for space */
	} else {
		idx = free_slot();
		if (idx < 0)
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
	kstrlcpy(dir[idx].name, leaf, FS_NAME_MAX);
	dir[idx].start = start;
	dir[idx].size = size;
	dir[idx].flags = FLAG_USED | ((uint32_t)par << 8);
	return flush_dir();

io_error:
	dir[idx] = saved;
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

int fs_delete(const char *path)
{
	int idx = fs_entry(path);

	if (idx < 0)
		return idx;
	if (idx == (int)ROOT || is_dir(idx))
		return FS_EISDIR;
	memset(&dir[idx], 0, sizeof dir[idx]);
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
		memset(&dir[victim], 0, sizeof dir[victim]); /* replace the old file */
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
	uint32_t usedsec = DATA_START;
	int i;

	for (i = 0; i < FS_MAX_FILES; i++)
		if (used(i) && !is_dir(i))
			usedsec += sectors_for(dir[i].size);
	return ata_sectors() > usedsec ? ata_sectors() - usedsec : 0;
}

int fs_info(struct fs_info *out)
{
	uint32_t cand = DATA_START, total = ata_sectors(), best = 0;
	int i, moved;

	if (!mounted)
		return FS_ENOMOUNT;
	memset(out, 0, sizeof *out);
	out->total_sectors = total;
	out->used_sectors = DATA_START;
	for (i = 0; i < FS_MAX_FILES; i++) {
		if (!used(i)) {
			out->free_entries++;
		} else if (is_dir(i)) {
			out->dirs++;
		} else {
			out->files++;
			out->used_sectors += sectors_for(dir[i].size);
		}
	}
	out->free_sectors = total > out->used_sectors ? total - out->used_sectors : 0;

	/* Largest hole: sweep the extents in address order. */
	for (;;) {
		uint32_t next = total; /* start of the next file after 'cand' */

		moved = 0;
		for (i = 0; i < FS_MAX_FILES; i++) {
			uint32_t s, e;

			if (!used(i) || is_dir(i) || !dir[i].size)
				continue;
			s = dir[i].start;
			e = s + sectors_for(dir[i].size);
			if (cand >= s && cand < e) {
				cand = e;
				moved = 1;
			} else if (s >= cand && s < next) {
				next = s;
			}
		}
		if (moved)
			continue;
		if (next - cand > best)
			best = next - cand;
		if (next >= total)
			break;
		cand = next;
	}
	out->largest_free = best;
	return FS_OK;
}
