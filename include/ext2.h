#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>

struct blkdev;

/* The fields of the ext2 superblock the driver cares about. */
struct ext2_super {
	uint32_t inodes, blocks, free_blocks, free_inodes, first_data_block;
	uint32_t block_size, blocks_per_group, inodes_per_group, revision, groups;
	uint16_t state, inode_size;
	char label[17];
};

int ext2_read_super(struct blkdev *dev, struct ext2_super *sb);   /* 0, FS_ENOMOUNT if there is no ext2 volume */

/* shell commands */
int cmd_ext2info(int argc, char **argv);   /* ext2info DEVICE */

#endif
