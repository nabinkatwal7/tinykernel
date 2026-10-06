#include "blk.h"

#include "ata.h"
#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "vfs.h"

static struct blkdev devs[BLK_MAX];

struct blkdev *blk_find(const char *name)
{
	int i;

	for (i = 0; i < BLK_MAX; i++)
		if (devs[i].used && !kstrcmp(devs[i].name, name))
			return &devs[i];
	return 0;
}

struct blkdev *blk_get(int i)
{
	return i >= 0 && i < BLK_MAX && devs[i].used ? &devs[i] : 0;
}

static struct blkdev *slot(const char *name)
{
	int i;

	if (!name[0] || kstrlen(name) >= BLK_NAME || blk_find(name))
		return 0;
	for (i = 0; i < BLK_MAX; i++) {
		if (!devs[i].used) {
			memset(&devs[i], 0, sizeof devs[i]);
			kstrlcpy(devs[i].name, name, BLK_NAME);
			devs[i].used = 1;
			return &devs[i];
		}
	}
	return 0;
}

int blk_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf)
{
	if (!d || !d->read || lba + n > d->sectors || lba + n < lba)
		return FS_EINVAL;
	return d->read(d, lba, n, buf);
}

int blk_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf)
{
	if (!d || !d->write || lba + n > d->sectors || lba + n < lba)
		return FS_EINVAL;
	return d->write(d, lba, n, buf);
}

/* ---- ATA drives ---- */

static int ata_blk_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf)
{
	return ata_dev_read((int)(uintptr_t)d->priv, lba, n, buf) ? FS_EIO : FS_OK;
}

static int ata_blk_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf)
{
	return ata_dev_write((int)(uintptr_t)d->priv, lba, n, buf) ? FS_EIO : FS_OK;
}

void blk_init(void)
{
	int i;

	for (i = 0; i < ATA_DEVICES; i++) {
		char name[BLK_NAME];
		struct blkdev *d;

		if (!ata_dev_present(i))
			continue;
		ksnprintf(name, sizeof name, "hd%d", i);
		d = slot(name);
		if (!d)
			continue;
		d->sectors = ata_dev_sectors(i);
		d->read = ata_blk_read;
		d->write = ata_blk_write;
		d->priv = (void *)(uintptr_t)i;
		d->kind = "ata";
	}
}

/* ---- RAM disks ---- */

static int ram_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf)
{
	memcpy(buf, (uint8_t *)d->priv + (size_t)lba * 512, (size_t)n * 512);
	return FS_OK;
}

static int ram_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf)
{
	memcpy((uint8_t *)d->priv + (size_t)lba * 512, buf, (size_t)n * 512);
	return FS_OK;
}

int ramdisk_create(const char *name, uint32_t kib)
{
	struct blkdev *d;
	void *mem;

	if (!kib || kib > 1024)
		return FS_EINVAL; /* up to 1 MiB: it lives in the kernel heap */
	mem = kcalloc(kib, 1024);
	if (!mem)
		return FS_ENOSPC;
	d = slot(name);
	if (!d) {
		kfree(mem);
		return FS_EEXIST;
	}
	d->sectors = kib * 2;
	d->read = ram_read;
	d->write = ram_write;
	d->priv = mem;
	d->kind = "ram";
	return FS_OK;
}

/* ---- loopback: a file as a disk ---- */

struct loop {
	char path[VFS_PATH_MAX];
	uint8_t *data;
	int dirty;
};

static int loop_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf)
{
	struct loop *l = d->priv;

	memcpy(buf, l->data + (size_t)lba * 512, (size_t)n * 512);
	return FS_OK;
}

static int loop_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf)
{
	struct loop *l = d->priv;

	memcpy(l->data + (size_t)lba * 512, buf, (size_t)n * 512);
	l->dirty = 1;
	return FS_OK;
}

int loop_attach(const char *name, const char *path)
{
	struct loop *l;
	struct blkdev *d;
	int size, n;

	size = vfs_size(path);
	if (size < 0)
		return size;
	if (size < 512 || size > 2 * 1024 * 1024)
		return FS_EINVAL;
	l = kcalloc(1, sizeof *l);
	if (!l)
		return FS_ENOSPC;
	l->data = kcalloc(1, ((size_t)size + 511) & ~(size_t)511);
	if (!l->data) {
		kfree(l);
		return FS_ENOSPC;
	}
	n = vfs_read(path, l->data, (uint32_t)size);
	if (n < 0) {
		kfree(l->data);
		kfree(l);
		return n;
	}
	d = slot(name);
	if (!d) {
		kfree(l->data);
		kfree(l);
		return FS_EEXIST;
	}
	kstrlcpy(l->path, path, sizeof l->path);
	d->sectors = (uint32_t)(size / 512);
	d->read = loop_read;
	d->write = loop_write;
	d->priv = l;
	d->kind = "loop";
	return FS_OK;
}

int loop_sync(const char *name)
{
	struct blkdev *d = blk_find(name);
	struct loop *l;
	int rc;

	if (!d || kstrcmp(d->kind, "loop"))
		return FS_ENOENT;
	l = d->priv;
	if (!l->dirty)
		return FS_OK;
	rc = vfs_write(l->path, l->data, d->sectors * 512);
	if (!rc)
		l->dirty = 0;
	return rc;
}

int blk_remove(const char *name)
{
	struct blkdev *d = blk_find(name);
	int i;

	if (!d || !kstrcmp(d->kind, "ata") || !kstrcmp(d->kind, "part"))
		return FS_EINVAL;
	for (i = 0; i < BLK_MAX; i++) /* partitions go with their device */
		if (devs[i].used && devs[i].parent == d)
			devs[i].used = 0;
	if (!kstrcmp(d->kind, "loop")) {
		struct loop *l = d->priv;

		loop_sync(name);
		kfree(l->data);
		kfree(l);
	} else {
		kfree(d->priv);
	}
	d->used = 0;
	return FS_OK;
}

/* ---- partitions ---- */

static int part_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf)
{
	return blk_read(d->parent, d->offset + lba, n, buf);
}

static int part_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf)
{
	return blk_write(d->parent, d->offset + lba, n, buf);
}

int part_read_table(struct blkdev *d, struct part_entry out[4])
{
	uint8_t sec[512];
	int i, n = 0;

	if (blk_read(d, 0, 1, sec) || sec[510] != 0x55 || sec[511] != 0xAA)
		return FS_EINVAL;
	for (i = 0; i < 4; i++) {
		const uint8_t *e = sec + 446 + i * 16;

		out[i].boot = e[0];
		out[i].type = e[4];
		out[i].start = (uint32_t)e[8] | (uint32_t)e[9] << 8 | (uint32_t)e[10] << 16 | (uint32_t)e[11] << 24;
		out[i].sectors = (uint32_t)e[12] | (uint32_t)e[13] << 8 | (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;
		if (out[i].type && out[i].sectors)
			n++;
		else
			out[i].type = 0;
	}
	return n;
}

int part_scan(struct blkdev *d)
{
	struct part_entry pe[4];
	int i, n = part_read_table(d, pe), made = 0;

	if (n < 0)
		return n;
	for (i = 0; i < 4; i++) {
		char name[BLK_NAME];
		struct blkdev *p;

		if (!pe[i].type || pe[i].start + pe[i].sectors > d->sectors)
			continue;
		ksnprintf(name, sizeof name, "%sp%d", d->name, i + 1);
		p = slot(name);
		if (!p)
			continue;
		p->sectors = pe[i].sectors;
		p->read = part_read;
		p->write = part_write;
		p->parent = d;
		p->offset = pe[i].start;
		p->kind = "part";
		made++;
	}
	return made;
}

int part_write_demo(struct blkdev *d)
{
	uint8_t sec[512];
	uint32_t half = d->sectors / 2;
	static const uint8_t types[2] = { 0x83, 0x0C };
	int i;

	if (d->sectors < 64)
		return FS_EINVAL;
	memset(sec, 0, sizeof sec);
	for (i = 0; i < 2; i++) {
		uint8_t *e = sec + 446 + i * 16;
		uint32_t start = i ? half : 8, count = i ? d->sectors - half : half - 8;

		e[0] = i ? 0 : 0x80;
		e[4] = types[i];
		memcpy(e + 8, &start, 4);
		memcpy(e + 12, &count, 4);
	}
	sec[510] = 0x55;
	sec[511] = 0xAA;
	return blk_write(d, 0, 1, sec);
}
