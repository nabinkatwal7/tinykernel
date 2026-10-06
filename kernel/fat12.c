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
	if (nclusters >= 4085) /* that would be FAT16 */
		return FS_ENOMOUNT;
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
	klog(LOG_INFO, "fat12: drive %d: %u clusters of %u bytes", ata_dev, nclusters, cluster_bytes);
	return FS_OK;
}

/* Next cluster in a chain: 0xFF8+ means end, 0xFF7 bad, 0 free. */
static uint32_t fat_next(uint32_t c)
{
	uint16_t w = rd16(fat + c + c / 2);

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
	for (c = cluster; c >= 2 && c < 0xFF8 && n < 256; c = fat_next(c))
		n++;
	buf = kmalloc(n * cluster_bytes);
	if (!buf)
		return 0;
	for (n = 0, c = cluster; c >= 2 && c < 0xFF8 && n < 256; c = fat_next(c), n++) {
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
	for (c = d.cluster; c >= 2 && c < 0xFF8 && got < d.size; c = fat_next(c)) {
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
