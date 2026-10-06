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
