#include "ext2.h"

#include "blk.h"
#include "console.h"
#include "fs.h"
#include "kmalloc.h"
#include "kstring.h"

/*
 * ext2 (see docs/storage.md for the on-disk layout). Everything is read through a block device in whole
 * file-system blocks; the superblock is always 1024 bytes into the volume.
 */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* Reads and checks the superblock of the volume on 'dev'. */
int ext2_read_super(struct blkdev *dev, struct ext2_super *sb)
{
	uint8_t raw[1024];

	if (blk_read(dev, 2, 2, raw)) /* bytes 1024..2047 */
		return FS_EIO;
	if (rd16(raw + 56) != 0xEF53)
		return FS_ENOMOUNT;
	memset(sb, 0, sizeof *sb);
	sb->inodes = rd32(raw + 0);
	sb->blocks = rd32(raw + 4);
	sb->free_blocks = rd32(raw + 12);
	sb->free_inodes = rd32(raw + 16);
	sb->first_data_block = rd32(raw + 20);
	sb->block_size = 1024u << rd32(raw + 24);
	sb->blocks_per_group = rd32(raw + 32);
	sb->inodes_per_group = rd32(raw + 40);
	sb->state = rd16(raw + 58);
	sb->revision = rd32(raw + 76);
	sb->inode_size = sb->revision >= 1 ? rd16(raw + 88) : 128;
	memcpy(sb->label, raw + 120, 16);
	sb->label[16] = '\0';
	if (sb->block_size > 4096 || !sb->blocks_per_group || !sb->inodes_per_group || sb->inode_size < 128
	    || sb->inode_size > sb->block_size)
		return FS_ENOMOUNT;
	sb->groups = (sb->blocks - sb->first_data_block + sb->blocks_per_group - 1) / sb->blocks_per_group;
	return FS_OK;
}

/* ext2info DEVICE : explain what the superblock and the group descriptors say */
int cmd_ext2info(int argc, char **argv)
{
	struct blkdev *dev;
	struct ext2_super sb;
	uint8_t *gd;
	uint32_t g, gd_block, gd_bytes;
	int rc;

	if (argc != 2) {
		console_write("usage: ext2info DEVICE\n");
		return 1;
	}
	dev = blk_find(argv[1]);
	if (!dev) {
		console_printf("%s: no such block device\n", argv[1]);
		return 1;
	}
	rc = ext2_read_super(dev, &sb);
	if (rc) {
		console_printf("%s: not an ext2 volume (%s)\n", argv[1], fs_strerror(rc));
		return 1;
	}
	console_printf("volume '%s': revision %u, %s\n", sb.label, sb.revision, sb.state & 1 ? "clean" : "not cleanly unmounted");
	console_printf("  block size %u, %u blocks (%u free), %u inodes (%u free), inode size %u\n", sb.block_size, sb.blocks,
		       sb.free_blocks, sb.inodes, sb.free_inodes, sb.inode_size);
	console_printf("  %u group(s) of %u blocks / %u inodes; first data block %u\n", sb.groups, sb.blocks_per_group,
		       sb.inodes_per_group, sb.first_data_block);
	gd_block = sb.first_data_block + 1; /* the descriptor table follows the superblock's block */
	gd_bytes = sb.groups * 32;
	gd = kmalloc((gd_bytes + sb.block_size) & ~(sb.block_size - 1));
	if (!gd)
		return 1;
	if (blk_read(dev, gd_block * (sb.block_size / 512), (uint32_t)((gd_bytes + 511) / 512), gd)) {
		kfree(gd);
		return 1;
	}
	for (g = 0; g < sb.groups; g++) {
		const uint8_t *e = gd + g * 32;

		console_printf("  group %u: block bitmap @%u, inode bitmap @%u, inode table @%u, %u free blocks, %u free inodes, %u dirs\n", g,
			       rd32(e), rd32(e + 4), rd32(e + 8), rd16(e + 12), rd16(e + 14), rd16(e + 16));
	}
	kfree(gd);
	return 0;
}

/* ======================= the read-only driver ======================= */

#include "vfs.h"

#define EXT2_ROOT 2
#define MAX_VOLUMES 2

struct ext2_inode {
	uint16_t mode, uid, gid, links;
	uint32_t size, mtime, blocks_512;
	uint32_t block[15];
};

struct ext2 {
	struct blkdev *dev;
	struct ext2_super sb;
	uint8_t *groups;            /* the group descriptor table */
	uint8_t *buf;               /* one block of scratch space */
	uint8_t *buf2;              /* a second one, for indirect blocks */
	int used;
	char mount[VFS_NAME_MAX];
};

static struct ext2 volumes[MAX_VOLUMES];

static int read_block(struct ext2 *e, uint32_t n, void *buf)
{
	uint32_t spb = e->sb.block_size / 512;

	return blk_read(e->dev, n * spb, spb, buf) ? FS_EIO : FS_OK;
}

static int read_inode(struct ext2 *e, uint32_t ino, struct ext2_inode *out)
{
	uint32_t group, index, table, offset, i;
	const uint8_t *raw;

	if (!ino || ino > e->sb.inodes)
		return FS_ENOENT;
	group = (ino - 1) / e->sb.inodes_per_group;
	index = (ino - 1) % e->sb.inodes_per_group;
	table = rd32(e->groups + group * 32 + 8);
	offset = index * e->sb.inode_size;
	if (read_block(e, table + offset / e->sb.block_size, e->buf))
		return FS_EIO;
	raw = e->buf + offset % e->sb.block_size;
	out->mode = (uint16_t)rd16(raw);
	out->uid = (uint16_t)rd16(raw + 2);
	out->size = rd32(raw + 4);
	out->mtime = rd32(raw + 16);
	out->gid = (uint16_t)rd16(raw + 24);
	out->links = (uint16_t)rd16(raw + 26);
	out->blocks_512 = rd32(raw + 28);
	for (i = 0; i < 15; i++)
		out->block[i] = rd32(raw + 40 + i * 4);
	return FS_OK;
}

/* Block number of the idx-th block of a file (0 = a hole). Direct, single and double indirect blocks. */
static int file_block(struct ext2 *e, const struct ext2_inode *in, uint32_t idx, uint32_t *out)
{
	uint32_t per = e->sb.block_size / 4;

	if (idx < 12) {
		*out = in->block[idx];
		return FS_OK;
	}
	idx -= 12;
	if (idx < per) {
		if (!in->block[12]) {
			*out = 0;
			return FS_OK;
		}
		if (read_block(e, in->block[12], e->buf2))
			return FS_EIO;
		*out = rd32(e->buf2 + idx * 4);
		return FS_OK;
	}
	idx -= per;
	if (idx < per * per) {
		uint32_t first;

		if (!in->block[13]) {
			*out = 0;
			return FS_OK;
		}
		if (read_block(e, in->block[13], e->buf2))
			return FS_EIO;
		first = rd32(e->buf2 + (idx / per) * 4);
		if (!first) {
			*out = 0;
			return FS_OK;
		}
		if (read_block(e, first, e->buf2))
			return FS_EIO;
		*out = rd32(e->buf2 + (idx % per) * 4);
		return FS_OK;
	}
	return FS_ETOOBIG; /* triple indirect: not supported */
}

/* Copy up to cap bytes of the file starting at off into buf; returns the byte count. */
static int read_data(struct ext2 *e, const struct ext2_inode *in, uint32_t off, void *buf, uint32_t cap)
{
	uint8_t *dst = buf;
	uint32_t bs = e->sb.block_size, done = 0;

	if (off >= in->size)
		return 0;
	if (cap > in->size - off)
		cap = in->size - off;
	while (done < cap) {
		uint32_t pos = off + done, blk, within = pos % bs, n = bs - within;
		int rc = file_block(e, in, pos / bs, &blk);

		if (rc)
			return rc;
		if (n > cap - done)
			n = cap - done;
		if (!blk) {
			memset(dst + done, 0, n); /* a hole reads as zeros */
		} else {
			if (read_block(e, blk, e->buf))
				return FS_EIO;
			memcpy(dst + done, e->buf + within, n);
		}
		done += n;
	}
	return (int)done;
}

/* Calls fn for each directory entry; fn returns nonzero to stop. */
typedef int (*dir_fn)(void *arg, uint32_t ino, const char *name, int type);

static int walk_dir(struct ext2 *e, const struct ext2_inode *dir, dir_fn fn, void *arg)
{
	uint32_t bs = e->sb.block_size, pos;
	uint8_t *blk = kmalloc(bs);
	int rc = FS_OK;

	if (!blk)
		return FS_ENOSPC;
	for (pos = 0; pos < dir->size && !rc; pos += bs) {
		uint32_t bn, off = 0;

		rc = file_block(e, dir, pos / bs, &bn);
		if (rc || !bn || read_block(e, bn, blk)) {
			rc = rc ? rc : FS_EIO;
			break;
		}
		while (off + 8 <= bs) {
			uint32_t ino = rd32(blk + off), len = rd16(blk + off + 4), nlen = blk[off + 6];
			char name[256];

			if (len < 8 || off + len > bs || nlen > len - 8)
				break; /* damaged block */
			if (ino) {
				memcpy(name, blk + off + 8, nlen);
				name[nlen] = '\0';
				if (fn(arg, ino, name, blk[off + 7])) {
					kfree(blk);
					return FS_OK;
				}
			}
			off += len;
		}
	}
	kfree(blk);
	return rc;
}

struct find {
	const char *name;
	uint32_t ino;
};

static int find_cb(void *arg, uint32_t ino, const char *name, int type)
{
	struct find *f = arg;

	(void)type;
	if (!kstrcmp(name, f->name)) {
		f->ino = ino;
		return 1;
	}
	return 0;
}

/* Inode number of a path relative to the volume root. */
static int lookup(struct ext2 *e, const char *path, uint32_t *ino_out)
{
	uint32_t ino = EXT2_ROOT;

	for (;;) {
		char comp[256];
		struct ext2_inode in;
		struct find f;
		size_t n = 0;
		int rc;

		while (*path == '/')
			path++;
		if (!*path) {
			*ino_out = ino;
			return FS_OK;
		}
		while (path[n] && path[n] != '/')
			n++;
		if (n >= sizeof comp)
			return FS_ENOENT;
		memcpy(comp, path, n);
		comp[n] = '\0';
		path += n;
		rc = read_inode(e, ino, &in);
		if (rc)
			return rc;
		if ((in.mode & 0xF000) != 0x4000)
			return FS_ENOTDIR;
		f.name = comp;
		f.ino = 0;
		rc = walk_dir(e, &in, find_cb, &f);
		if (rc)
			return rc;
		if (!f.ino)
			return FS_ENOENT;
		ino = f.ino;
	}
}

/* ---- the VFS side ---- */

static int ext2_stat_op(void *ctx, const char *path, struct vfs_stat *st)
{
	struct ext2 *e = ctx;
	struct ext2_inode in;
	uint32_t ino;
	int rc = lookup(e, path, &ino);

	if (!rc)
		rc = read_inode(e, ino, &in);
	if (rc)
		return rc;
	st->size = in.size;
	st->is_dir = (in.mode & 0xF000) == 0x4000;
	st->is_link = (in.mode & 0xF000) == 0xA000;
	st->dev = 0;
	st->has_meta = 1;
	st->mode = in.mode & 0777;
	st->uid = in.uid;
	st->gid = in.gid;
	st->mtime = in.mtime;
	st->nlink = in.links;
	return FS_OK;
}

static int ext2_read_op(void *ctx, const char *path, void *buf, uint32_t cap)
{
	struct ext2 *e = ctx;
	struct ext2_inode in;
	uint32_t ino;
	int rc = lookup(e, path, &ino);

	if (!rc)
		rc = read_inode(e, ino, &in);
	if (rc)
		return rc;
	if ((in.mode & 0xF000) == 0x4000)
		return FS_EISDIR;
	if (in.size > cap)
		return FS_ETOOBIG;
	return read_data(e, &in, 0, buf, in.size);
}

struct list_ctx {
	struct ext2 *e;
	struct vfs_dirent *out;
	int max, n;
};

static int list_cb(void *arg, uint32_t ino, const char *name, int type)
{
	struct list_ctx *l = arg;
	struct ext2_inode in;
	struct vfs_dirent *d;

	(void)type;
	if (!kstrcmp(name, ".") || !kstrcmp(name, ".."))
		return 0;
	if (l->n >= l->max)
		return 1;
	d = &l->out[l->n];
	kstrlcpy(d->name, name, sizeof d->name);
	memset(&in, 0, sizeof in);
	if (!read_inode(l->e, ino, &in)) {
		d->size = in.size;
		d->is_dir = (in.mode & 0xF000) == 0x4000;
		d->is_link = (in.mode & 0xF000) == 0xA000;
		d->has_meta = 1;
		d->mode = in.mode & 0777;
		d->uid = in.uid;
		d->gid = in.gid;
		d->mtime = in.mtime;
	}
	l->n++;
	return 0;
}

static int ext2_list_op(void *ctx, const char *path, struct vfs_dirent *out, int max)
{
	struct ext2 *e = ctx;
	struct ext2_inode in;
	struct list_ctx l;
	uint32_t ino;
	int rc = lookup(e, path, &ino);

	if (!rc)
		rc = read_inode(e, ino, &in);
	if (rc)
		return rc;
	if ((in.mode & 0xF000) != 0x4000)
		return FS_ENOTDIR;
	l.e = e;
	l.out = out;
	l.max = max;
	l.n = 0;
	rc = walk_dir(e, &in, list_cb, &l);
	return rc ? rc : l.n;
}

/* Short symbolic links (under 60 bytes) keep the target in the block pointer area of the inode; longer ones use a data block. */
static int ext2_readlink_op(void *ctx, const char *path, char *buf, uint32_t cap)
{
	struct ext2 *e = ctx;
	struct ext2_inode in;
	uint32_t ino;
	int rc = lookup(e, path, &ino), n;

	if (!rc)
		rc = read_inode(e, ino, &in);
	if (rc)
		return rc;
	if ((in.mode & 0xF000) != 0xA000 || !cap)
		return FS_EINVAL;
	if (in.size < 60) {
		n = (int)(in.size < cap - 1 ? in.size : cap - 1);
		memcpy(buf, in.block, (size_t)n);
	} else {
		n = read_data(e, &in, 0, buf, cap - 1);
		if (n < 0)
			return n;
	}
	buf[n] = '\0';
	return n;
}

static const struct vfs_ops ext2_ops = {
	"ext2", ext2_stat_op, ext2_read_op, 0, 0, 0, ext2_list_op, 0, 0, 0,
	0, 0, 0, 0, ext2_readlink_op, 0,
};

int ext2_mount(const char *devname, const char *where)
{
	struct blkdev *dev = blk_find(devname);
	struct ext2 *e = 0;
	uint32_t gd_bytes;
	int i, rc;

	if (!dev)
		return FS_ENOENT;
	for (i = 0; i < MAX_VOLUMES && !e; i++)
		if (!volumes[i].used)
			e = &volumes[i];
	if (!e)
		return FS_ENOSPC;
	memset(e, 0, sizeof *e);
	rc = ext2_read_super(dev, &e->sb);
	if (rc)
		return rc;
	e->dev = dev;
	gd_bytes = (e->sb.groups * 32 + 511) & ~511u;
	e->groups = kmalloc(gd_bytes);
	e->buf = kmalloc(e->sb.block_size);
	e->buf2 = kmalloc(e->sb.block_size);
	if (!e->groups || !e->buf || !e->buf2 ||
	    blk_read(dev, (e->sb.first_data_block + 1) * (e->sb.block_size / 512), gd_bytes / 512, e->groups)) {
		kfree(e->groups);
		kfree(e->buf);
		kfree(e->buf2);
		return FS_EIO;
	}
	kstrlcpy(e->mount, where, sizeof e->mount);
	rc = vfs_mount(where, &ext2_ops, e);
	if (rc) {
		kfree(e->groups);
		kfree(e->buf);
		kfree(e->buf2);
		return rc;
	}
	e->used = 1;
	return FS_OK;
}

int ext2_umount(const char *where)
{
	int i, rc = FS_ENOENT;

	for (i = 0; i < MAX_VOLUMES; i++) {
		struct ext2 *e = &volumes[i];

		if (e->used && !kstrcmp(e->mount, where)) {
			rc = vfs_unmount(where);
			if (rc)
				return rc;
			kfree(e->groups);
			kfree(e->buf);
			kfree(e->buf2);
			e->used = 0;
			return FS_OK;
		}
	}
	return rc;
}

/* ext2 mount DEVICE /PATH | ext2 umount /PATH */
int cmd_ext2(int argc, char **argv)
{
	int rc;

	if (argc == 4 && !kstrcmp(argv[1], "mount")) {
		rc = ext2_mount(argv[2], argv[3]);
	} else if (argc == 3 && !kstrcmp(argv[1], "umount")) {
		rc = ext2_umount(argv[2]);
	} else if (argc == 2 && !kstrcmp(argv[1], "test")) { /* loop device -> partition -> mount -> read -> unmount */
		static char big[80000];
		char small[64];
		int bad = 0, n;

		if (loop_attach("exttest", "/fat/EXT2.IMG") || part_scan(blk_find("exttest")) != 1)
			bad++;
		else if (ext2_mount("exttestp1", "/exttest"))
			bad++;
		else {
			n = vfs_read("/exttest/hello.txt", small, sizeof small - 1);
			bad += n != 16 || memcmp(small, "hello from ext2", 15);
			n = vfs_read("/exttest/docs/note.txt", small, sizeof small - 1);
			bad += n != 53;
			n = vfs_read("/exttest/big.txt", big, sizeof big); /* needs the indirect block */
			bad += n != 73500 || memcmp(big + 73500 - 49 + 14, "line 1499", 9);
			bad += vfs_size("/exttest/missing") >= 0;
			bad += ext2_umount("/exttest") != 0;
		}
		blk_remove("exttest");
		console_printf("ext2 test: %d errors: %s\n", bad, bad ? "FAILED" : "ok");
		return bad != 0;
	} else {
		console_write("usage: ext2 mount DEVICE /PATH | ext2 umount /PATH\n");
		return 1;
	}
	if (rc)
		console_printf("ext2: %s\n", fs_strerror(rc));
	return rc != 0;
}
