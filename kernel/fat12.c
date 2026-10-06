#include "fat12.h"

#include "ata.h"
#include "fs.h"
#include "klog.h"
#include "kmalloc.h"
#include "kstring.h"

#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10
#define ATTR_LFN    0x0F

struct dent { /* raw 32-byte directory entry */
	uint8_t name[11];
	uint8_t attr;
	uint8_t reserved[14];
	uint16_t cluster;
	uint32_t size;
} __attribute__((packed));

static int dev = -1;
static uint32_t bytes_per_sector, spc, reserved, nfats, root_entries, total_sectors, spf;
static uint32_t root_start, root_sectors, data_start, nclusters, cluster_bytes;
static uint32_t eoc = 0xFF8;  /* first "end of chain" value: 0xFF8 (FAT12) or 0xFFF8 (FAT16) */
static int is16;            /* FAT16: 16-bit entries, read only */
static uint8_t *fat;        /* first FAT copy */
static uint8_t *root_dir;   /* root directory, read once */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int fat12_mounted(void)
{
	return dev >= 0;
}

void fat12_info(uint32_t *total, uint32_t *clusters, uint32_t *cbytes)
{
	*total = total_sectors;
	*clusters = nclusters;
	*cbytes = cluster_bytes;
}

int fat12_mount(int ata_dev)
{
	uint8_t bs[512];

	dev = -1;
	kfree(fat);
	kfree(root_dir);
	fat = root_dir = 0;
	if (!ata_dev_present(ata_dev) || ata_dev_read(ata_dev, 0, 1, bs))
		return FS_ENOMOUNT;
	if (bs[510] != 0x55 || bs[511] != 0xAA)
		return FS_ENOMOUNT;
	bytes_per_sector = rd16(bs + 11);
	spc = bs[13];
	reserved = rd16(bs + 14);
	nfats = bs[16];
	root_entries = rd16(bs + 17);
	total_sectors = rd16(bs + 19);
	if (!total_sectors)
		total_sectors = (uint32_t)rd16(bs + 32) | ((uint32_t)rd16(bs + 34) << 16);
	spf = rd16(bs + 22);
	if (bytes_per_sector != 512 || !spc || (spc & (spc - 1)) || !nfats || !spf || !root_entries)
		return FS_ENOMOUNT;

	root_start = reserved + nfats * spf;
	root_sectors = (root_entries * 32 + 511) / 512;
	data_start = root_start + root_sectors;
	if (total_sectors <= data_start)
		return FS_ENOMOUNT;
	nclusters = (total_sectors - data_start) / spc;
	if (nclusters >= 65525) /* FAT32 */
		return FS_ENOMOUNT;
	is16 = nclusters >= 4085; /* the cluster count decides, as the specification says */
	eoc = is16 ? 0xFFF8 : 0xFF8;
	cluster_bytes = spc * 512;

	fat = kmalloc(spf * 512);
	root_dir = kmalloc(root_sectors * 512);
	if (!fat || !root_dir || ata_dev_read(ata_dev, reserved, spf, fat)
	    || ata_dev_read(ata_dev, root_start, root_sectors, root_dir)) {
		kfree(fat);
		kfree(root_dir);
		fat = root_dir = 0;
		return FS_EIO;
	}
	dev = ata_dev;
	klog(LOG_INFO, "fat%d: drive %d: %u clusters of %u bytes%s", is16 ? 16 : 12, ata_dev, nclusters, cluster_bytes,
	     is16 ? " (read-only)" : "");
	return FS_OK;
}

/* Next cluster in a chain: eoc+ means end, 0xFF7 bad, 0 free. */
static uint32_t fat_next(uint32_t c)
{
	uint16_t w;

	if (is16)
		return rd16(fat + c * 2);
	w = rd16(fat + c + c / 2);
	return (c & 1) ? (uint32_t)(w >> 4) : (uint32_t)(w & 0x0FFF);
}

static int read_cluster(uint32_t c, void *buf)
{
	return ata_dev_read(dev, data_start + (c - 2) * spc, spc, buf);
}

/* "name.ext" -> 11 upper-case, space-padded bytes; -1 if it cannot be an 8.3 name. */
static int to_83(const char *comp, uint8_t out[11])
{
	const char *dot = 0, *p;
	size_t base, ext, i;

	for (p = comp; *p; p++)
		if (*p == '.')
			dot = p;
	base = dot ? (size_t)(dot - comp) : kstrlen(comp);
	ext = dot ? kstrlen(dot + 1) : 0;
	if (base == 0 || base > 8 || ext > 3)
		return -1;
	memset(out, ' ', 11);
	for (i = 0; i < base; i++)
		out[i] = (uint8_t)(comp[i] >= 'a' && comp[i] <= 'z' ? comp[i] - 32 : comp[i]);
	for (i = 0; i < ext; i++)
		out[8 + i] = (uint8_t)(dot[1 + i] >= 'a' && dot[1 + i] <= 'z' ? dot[1 + i] - 32 : dot[1 + i]);
	return 0;
}

static void from_83(const uint8_t n[11], char out[13])
{
	int i, o = 0;

	for (i = 0; i < 8 && n[i] != ' '; i++)
		out[o++] = (char)n[i];
	if (n[8] != ' ') {
		out[o++] = '.';
		for (i = 8; i < 11 && n[i] != ' '; i++)
			out[o++] = (char)n[i];
	}
	out[o] = '\0';
}

/*
 * Load the entries of a directory into a fresh buffer: cluster 0 = root. *count receives the number
 * of 32-byte slots. Caller kfree()s the result.
 */
static struct dent *load_dir(uint32_t cluster, int *count)
{
	uint8_t *buf;
	uint32_t n = 0, c;

	if (cluster == 0) {
		buf = kmalloc(root_entries * 32);
		if (buf)
			memcpy(buf, root_dir, root_entries * 32);
		*count = (int)root_entries;
		return (struct dent *)buf;
	}
	for (c = cluster; c >= 2 && c < eoc && n < 256; c = fat_next(c))
		n++;
	buf = kmalloc(n * cluster_bytes);
	if (!buf)
		return 0;
	for (n = 0, c = cluster; c >= 2 && c < eoc && n < 256; c = fat_next(c), n++) {
		if (read_cluster(c, buf + n * cluster_bytes)) {
			kfree(buf);
			return 0;
		}
	}
	*count = (int)(n * cluster_bytes / 32);
	return (struct dent *)buf;
}

/* Resolve a path to its directory entry (a synthetic one for the root). */
static int lookup(const char *path, struct dent *out)
{
	uint32_t cluster = 0; /* directory being searched; 0 = root */

	memset(out, 0, sizeof *out);
	out->attr = ATTR_DIR;
	for (;;) {
		char comp[16];
		uint8_t want[11];
		struct dent *d;
		int count, i, found = -1;
		size_t n = 0;

		while (*path == '/')
			path++;
		if (!*path)
			return FS_OK; /* the directory reached so far is the answer */
		while (path[n] && path[n] != '/')
			n++;
		if (n >= sizeof comp)
			return FS_ENOENT;
		kstrlcpy(comp, path, n + 1);
		if (to_83(comp, want))
			return FS_ENOENT;
		path += n;
		if (!(out->attr & ATTR_DIR))
			return FS_ENOTDIR;

		d = load_dir(cluster, &count);
		if (!d)
			return FS_EIO;
		for (i = 0; i < count && d[i].name[0] != 0x00; i++) {
			if (d[i].name[0] == 0xE5 || d[i].attr == ATTR_LFN || (d[i].attr & ATTR_VOLUME))
				continue;
			if (!memcmp(d[i].name, want, 11)) {
				found = i;
				break;
			}
		}
		if (found >= 0)
			*out = d[found];
		kfree(d);
		if (found < 0)
			return FS_ENOENT;
		cluster = out->cluster;
	}
}

int fat12_stat(const char *path, struct fat12_entry *out)
{
	struct dent d;
	int rc;

	if (dev < 0)
		return FS_ENOMOUNT;
	rc = lookup(path, &d);
	if (rc)
		return rc;
	if (!*path || !kstrcmp(path, "/")) {
		kstrlcpy(out->name, "/", sizeof out->name);
	} else {
		from_83(d.name, out->name);
	}
	out->size = d.size;
	out->is_dir = (d.attr & ATTR_DIR) != 0;
	return FS_OK;
}

int fat12_read(const char *path, void *buf, uint32_t cap)
{
	struct dent d;
	uint8_t *tmp, *dst = buf;
	uint32_t c, got = 0;
	int rc;

	if (dev < 0)
		return FS_ENOMOUNT;
	rc = lookup(path, &d);
	if (rc)
		return rc;
	if (d.attr & ATTR_DIR)
		return FS_EISDIR;
	if (d.size > cap)
		return FS_ETOOBIG;
	if (!d.size)
		return 0;
	tmp = kmalloc(cluster_bytes);
	if (!tmp)
		return FS_ENOSPC;
	for (c = d.cluster; c >= 2 && c < eoc && got < d.size; c = fat_next(c)) {
		uint32_t n = d.size - got < cluster_bytes ? d.size - got : cluster_bytes;

		if (read_cluster(c, tmp)) {
			kfree(tmp);
			return FS_EIO;
		}
		memcpy(dst + got, tmp, n);
		got += n;
	}
	kfree(tmp);
	return got == d.size ? (int)got : FS_EIO; /* a short chain means a damaged file */
}

int fat12_list(const char *path, struct fat12_entry *out, int max)
{
	struct dent d, *dirbuf;
	int count, i, n = 0, rc;

	if (dev < 0)
		return FS_ENOMOUNT;
	rc = lookup(path, &d);
	if (rc)
		return rc;
	if (!(d.attr & ATTR_DIR))
		return FS_ENOTDIR;
	dirbuf = load_dir(d.cluster, &count);
	if (!dirbuf)
		return FS_EIO;
	for (i = 0; i < count && dirbuf[i].name[0] != 0x00 && n < max; i++) {
		struct dent *e = &dirbuf[i];

		if (e->name[0] == 0xE5 || e->attr == ATTR_LFN || (e->attr & ATTR_VOLUME))
			continue;
		from_83(e->name, out[n].name);
		out[n].size = e->size;
		out[n].is_dir = (e->attr & ATTR_DIR) != 0;
		n++;
	}
	kfree(dirbuf);
	return n;
}

/* ======================= writing ======================= */

static void fat_set(uint32_t c, uint32_t v)
{
	uint16_t w = rd16(fat + c + c / 2);

	if (c & 1)
		w = (uint16_t)((w & 0x000F) | (v << 4));
	else
		w = (uint16_t)((w & 0xF000) | (v & 0x0FFF));
	fat[c + c / 2] = (uint8_t)w;
	fat[c + c / 2 + 1] = (uint8_t)(w >> 8);
}

/* Write the FAT back to every copy on disk. */
static int flush_fat(void)
{
	uint32_t i;

	for (i = 0; i < nfats; i++)
		if (ata_dev_write(dev, reserved + i * spf, spf, fat))
			return FS_EIO;
	return FS_OK;
}

static void free_chain(uint32_t c)
{
	uint32_t guard = 0;

	while (c >= 2 && c < eoc && guard++ < 4096) {
		uint32_t next = fat_next(c);

		fat_set(c, 0);
		c = next;
	}
}

/* Allocate a chain of n clusters (first-fit); returns the first one, or 0 if the disk is full. Nothing is
   written to disk here. The clusters are not zeroed: callers write every one of them. */
static uint32_t alloc_chain(uint32_t n)
{
	uint32_t first = 0, prev = 0, c, got = 0;

	if (!n)
		return 0;
	for (c = 2; c < nclusters + 2 && got < n; c++) {
		if (fat_next(c))
			continue;
		if (prev)
			fat_set(prev, c);
		else
			first = c;
		fat_set(c, 0xFFF); /* provisional end of chain */
		prev = c;
		got++;
	}
	if (got < n) {
		free_chain(first);
		return 0;
	}
	return first;
}

/* Write a whole directory (as returned by load_dir) back to disk. */
static int store_dir(uint32_t cluster, const struct dent *d)
{
	uint32_t c, n = 0;

	if (cluster == 0) {
		memcpy(root_dir, d, root_entries * 32);
		return ata_dev_write(dev, root_start, root_sectors, root_dir) ? FS_EIO : FS_OK;
	}
	for (c = cluster; c >= 2 && c < eoc && n < 256; c = fat_next(c), n++)
		if (ata_dev_write(dev, data_start + (c - 2) * spc, spc, (const uint8_t *)d + n * cluster_bytes))
			return FS_EIO;
	return FS_OK;
}

/* The directory that holds the last component of path (its cluster, 0 = root) and that component as an 8.3 name. */
static int split_path(const char *path, uint32_t *dircluster, uint8_t name[11])
{
	char parent[64], comp[16];
	const char *slash = 0, *p;
	struct dent d;
	int rc;
	size_t n;

	for (p = path; *p; p++)
		if (*p == '/' && p[1])
			slash = p;
	if (!slash) { /* "name" or "/name" */
		kstrlcpy(parent, "/", sizeof parent);
		kstrlcpy(comp, path + (*path == '/'), sizeof comp);
	} else {
		n = (size_t)(slash - path);
		if (n >= sizeof parent || kstrlen(slash + 1) >= sizeof comp)
			return FS_EINVAL;
		memcpy(parent, path, n);
		parent[n] = '\0';
		if (!n)
			kstrlcpy(parent, "/", sizeof parent);
		kstrlcpy(comp, slash + 1, sizeof comp);
	}
	n = kstrlen(comp);
	while (n && comp[n - 1] == '/')
		comp[--n] = '\0';
	if (!n || to_83(comp, name))
		return FS_EINVAL;
	rc = lookup(parent, &d);
	if (rc)
		return rc;
	if (!(d.attr & ATTR_DIR))
		return FS_ENOTDIR;
	*dircluster = d.cluster;
	return FS_OK;
}

/* Index of the entry called 'name' in directory buffer d (or -1), and of the first free slot (or -1). */
static void scan_dir(const struct dent *d, int count, const uint8_t name[11], int *found, int *slot)
{
	int i;

	*found = *slot = -1;
	for (i = 0; i < count; i++) {
		if (d[i].name[0] == 0x00) {
			if (*slot < 0)
				*slot = i;
			break; /* nothing after the end marker */
		}
		if (d[i].name[0] == 0xE5) {
			if (*slot < 0)
				*slot = i;
			continue;
		}
		if (d[i].attr == ATTR_LFN || (d[i].attr & ATTR_VOLUME))
			continue;
		if (!memcmp(d[i].name, name, 11)) {
			*found = i;
			break;
		}
	}
}

/*
 * Load directory 'cluster' ready for inserting 'name': *found / *slot as in scan_dir. A full subdirectory grows by one
 * zeroed cluster (the FAT in memory is changed; callers flush it). A full root cannot grow: *slot stays -1.
 */
static struct dent *load_dir_for_insert(uint32_t cluster, int *count, const uint8_t name[11], int *found, int *slot)
{
	struct dent *d = load_dir(cluster, count);

	if (!d)
		return 0;
	scan_dir(d, *count, name, found, slot);
	if (*slot < 0 && *found < 0 && cluster) {
		uint32_t last = cluster, extra, n = 0;
		uint8_t *bigger;

		while (fat_next(last) >= 2 && fat_next(last) < eoc && n++ < 256)
			last = fat_next(last);
		extra = alloc_chain(1);
		if (!extra) {
			kfree(d);
			return 0;
		}
		fat_set(last, extra);
		bigger = kcalloc(1, (size_t)(*count) * 32 + cluster_bytes);
		if (!bigger) {
			kfree(d);
			return 0;
		}
		memcpy(bigger, d, (size_t)(*count) * 32);
		*slot = *count;
		*count += (int)(cluster_bytes / 32);
		kfree(d);
		d = (struct dent *)bigger;
	}
	return d;
}

static void make_entry(struct dent *e, const uint8_t name[11], uint8_t attr, uint32_t cluster, uint32_t size)
{
	memset(e, 0, sizeof *e);
	memcpy(e->name, name, 11);
	e->attr = attr;
	e->cluster = (uint16_t)cluster;
	e->size = size;
}

int fat12_write(const char *path, const void *data, uint32_t size)
{
	uint32_t dircluster, first = 0, n = (size + cluster_bytes - 1) / cluster_bytes, c, i;
	uint8_t name[11], *bounce, *saved_fat;
	struct dent *d;
	int count, found, slot, rc, idx;
	const uint8_t *src = data;

	if (dev < 0)
		return FS_ENOMOUNT;
	if (is16)
		return FS_EROFS; /* only reading FAT16 is supported */
	rc = split_path(path, &dircluster, name);
	if (rc)
		return rc;
	saved_fat = kmalloc(spf * 512);
	bounce = kmalloc(cluster_bytes);
	if (!saved_fat || !bounce) {
		kfree(saved_fat);
		kfree(bounce);
		return FS_ENOSPC;
	}
	memcpy(saved_fat, fat, spf * 512);
	d = load_dir_for_insert(dircluster, &count, name, &found, &slot);
	if (!d) {
		memcpy(fat, saved_fat, spf * 512);
		kfree(saved_fat);
		kfree(bounce);
		return FS_ENOSPC;
	}
	idx = found >= 0 ? found : slot;
	if (idx < 0) {
		rc = FS_ENOSPC; /* the root directory is full */
		goto fail;
	}
	if (found >= 0) {
		if (d[found].attr & ATTR_DIR) {
			rc = FS_EISDIR;
			goto fail;
		}
		free_chain(d[found].cluster); /* replacing: the old clusters become free first */
	}
	if (n) {
		first = alloc_chain(n);
		if (!first) {
			rc = FS_ENOSPC;
			goto fail;
		}
		for (i = 0, c = first; i < n; i++, c = fat_next(c)) {
			uint32_t left = size - i * cluster_bytes, take = left < cluster_bytes ? left : cluster_bytes;

			memset(bounce, 0, cluster_bytes);
			memcpy(bounce, src + i * cluster_bytes, take);
			if (ata_dev_write(dev, data_start + (c - 2) * spc, spc, bounce)) {
				rc = FS_EIO;
				goto fail;
			}
		}
	}
	make_entry(&d[idx], name, 0x20, first, size);
	rc = store_dir(dircluster, d);
	if (!rc)
		rc = flush_fat();
	if (rc)
		goto fail;
	kfree(d);
	kfree(saved_fat);
	kfree(bounce);
	return FS_OK;
fail:
	memcpy(fat, saved_fat, spf * 512);
	kfree(d);
	kfree(saved_fat);
	kfree(bounce);
	return rc;
}

int fat12_delete(const char *path)
{
	uint32_t dircluster;
	uint8_t name[11];
	struct dent *d;
	int count, found, slot, rc;

	if (dev < 0)
		return FS_ENOMOUNT;
	if (is16)
		return FS_EROFS; /* only reading FAT16 is supported */
	rc = split_path(path, &dircluster, name);
	if (rc)
		return rc;
	d = load_dir(dircluster, &count);
	if (!d)
		return FS_EIO;
	scan_dir(d, count, name, &found, &slot);
	if (found < 0) {
		kfree(d);
		return FS_ENOENT;
	}
	if (d[found].attr & ATTR_DIR) {
		kfree(d);
		return FS_EISDIR;
	}
	free_chain(d[found].cluster);
	d[found].name[0] = 0xE5;
	rc = store_dir(dircluster, d);
	if (!rc)
		rc = flush_fat();
	kfree(d);
	return rc;
}

int fat12_mkdir(const char *path)
{
	uint32_t dircluster, c;
	uint8_t name[11], *buf;
	struct dent *d, *dots;
	int count, found, slot, rc;

	if (dev < 0)
		return FS_ENOMOUNT;
	if (is16)
		return FS_EROFS; /* only reading FAT16 is supported */
	rc = split_path(path, &dircluster, name);
	if (rc)
		return rc;
	d = load_dir_for_insert(dircluster, &count, name, &found, &slot);
	if (!d)
		return FS_ENOSPC;
	if (found >= 0 || slot < 0) {
		kfree(d);
		return found >= 0 ? FS_EEXIST : FS_ENOSPC;
	}
	c = alloc_chain(1);
	buf = kcalloc(1, cluster_bytes);
	if (!c || !buf) {
		if (c)
			free_chain(c);
		kfree(d);
		kfree(buf);
		return FS_ENOSPC;
	}
	dots = (struct dent *)buf;
	memset(dots[0].name, ' ', 11);
	dots[0].name[0] = '.';
	dots[0].attr = ATTR_DIR;
	dots[0].cluster = (uint16_t)c;
	memset(dots[1].name, ' ', 11);
	dots[1].name[0] = '.';
	dots[1].name[1] = '.';
	dots[1].attr = ATTR_DIR;
	dots[1].cluster = (uint16_t)dircluster;
	rc = ata_dev_write(dev, data_start + (c - 2) * spc, spc, buf) ? FS_EIO : FS_OK;
	if (!rc) {
		make_entry(&d[slot], name, ATTR_DIR, c, 0);
		rc = store_dir(dircluster, d);
	}
	if (!rc)
		rc = flush_fat();
	kfree(buf);
	kfree(d);
	return rc;
}

int fat12_rmdir(const char *path)
{
	uint32_t dircluster;
	uint8_t name[11];
	struct dent *d, *sub;
	int count, found, slot, rc, subcount, i;

	if (dev < 0)
		return FS_ENOMOUNT;
	if (is16)
		return FS_EROFS; /* only reading FAT16 is supported */
	rc = split_path(path, &dircluster, name);
	if (rc)
		return rc;
	d = load_dir(dircluster, &count);
	if (!d)
		return FS_EIO;
	scan_dir(d, count, name, &found, &slot);
	if (found < 0 || !(d[found].attr & ATTR_DIR)) {
		rc = found < 0 ? FS_ENOENT : FS_ENOTDIR;
		kfree(d);
		return rc;
	}
	sub = load_dir(d[found].cluster, &subcount);
	if (!sub) {
		kfree(d);
		return FS_EIO;
	}
	for (i = 2; i < subcount && sub[i].name[0] != 0x00; i++) {
		if (sub[i].name[0] != 0xE5) {
			kfree(sub);
			kfree(d);
			return FS_ENOTEMPTY;
		}
	}
	kfree(sub);
	free_chain(d[found].cluster);
	d[found].name[0] = 0xE5;
	rc = store_dir(dircluster, d);
	if (!rc)
		rc = flush_fat();
	kfree(d);
	return rc;
}

/* Rename or move a file within the volume (directories are not supported). An existing file at the destination is replaced. */
int fat12_rename(const char *from, const char *to)
{
	uint32_t fc, tc;
	uint8_t fname[11], tname[11];
	struct dent *fd, *td = 0;
	int fcount, tcount, ffound, fslot, tfound, tslot, rc;

	if (dev < 0)
		return FS_ENOMOUNT;
	if (is16)
		return FS_EROFS; /* only reading FAT16 is supported */
	rc = split_path(from, &fc, fname);
	if (!rc)
		rc = split_path(to, &tc, tname);
	if (rc)
		return rc;
	fd = load_dir(fc, &fcount);
	if (!fd)
		return FS_EIO;
	scan_dir(fd, fcount, fname, &ffound, &fslot);
	if (ffound < 0 || (fd[ffound].attr & ATTR_DIR)) {
		rc = ffound < 0 ? FS_ENOENT : FS_EINVAL;
		kfree(fd);
		return rc;
	}
	if (fc == tc) { /* same directory: just change the name in place */
		scan_dir(fd, fcount, tname, &tfound, &tslot);
		if (tfound == ffound) {
			kfree(fd);
			return FS_OK;
		}
		if (tfound >= 0) {
			if (fd[tfound].attr & ATTR_DIR) {
				kfree(fd);
				return FS_EEXIST;
			}
			free_chain(fd[tfound].cluster);
			fd[tfound].name[0] = 0xE5;
		}
		memcpy(fd[ffound].name, tname, 11);
		rc = store_dir(fc, fd);
		if (!rc)
			rc = flush_fat();
		kfree(fd);
		return rc;
	}
	td = load_dir_for_insert(tc, &tcount, tname, &tfound, &tslot);
	if (!td) {
		kfree(fd);
		return FS_ENOSPC;
	}
	if (tfound >= 0 && (td[tfound].attr & ATTR_DIR)) {
		rc = FS_EEXIST;
	} else if (tfound < 0 && tslot < 0) {
		rc = FS_ENOSPC;
	} else {
		int idx = tfound >= 0 ? tfound : tslot;

		if (tfound >= 0)
			free_chain(td[tfound].cluster);
		td[idx] = fd[ffound];
		memcpy(td[idx].name, tname, 11);
		fd[ffound].name[0] = 0xE5; /* the clusters now belong to the new name */
		rc = store_dir(tc, td);
		if (!rc)
			rc = store_dir(fc, fd);
		if (!rc)
			rc = flush_fat();
	}
	kfree(td);
	kfree(fd);
	return rc;
}
