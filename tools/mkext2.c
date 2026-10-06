/*
 * Host tool: build a small ext2 image (revision 1, 1 KiB blocks, one block group) from a directory tree.
 *   mkext2 out.img rootdir [mbr]
 * With "mbr" the file system sits in a single partition (type 0x83) that starts at sector 8, behind a
 * partition table, so the image can be used to test partition parsing too.
 * Files use 12 direct blocks and one indirect block (up to 268 KiB each). No symbolic links, no extended attributes.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BS          1024
#define NBLOCKS     256             /* 256 KiB file system */
#define NINODES     128
#define INODE_SIZE  128
#define ITABLE_BLKS (NINODES * INODE_SIZE / BS)
#define FIRST_DATA  (5 + ITABLE_BLKS)  /* boot, super, descriptors, two bitmaps, inode table */
#define MTIME       1700000000u

static uint8_t img[NBLOCKS * BS];
static int next_block = FIRST_DATA;
static int next_inode = 11; /* inodes 1-10 are reserved; 2 is the root */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint8_t *blockp(int b) { return img + (size_t)b * BS; }

static int alloc_block(void)
{
	if (next_block >= NBLOCKS) {
		fprintf(stderr, "mkext2: image full\n");
		exit(1);
	}
	return next_block++;
}

static uint8_t *inode(int ino) { return blockp(5) + (size_t)(ino - 1) * INODE_SIZE; }

/* Fill an inode that owns 'size' bytes in the given blocks (12 direct + one indirect block). */
static void set_inode(int ino, uint32_t mode, uint32_t size, const int *blocks, int nblocks, int links)
{
	uint8_t *i = inode(ino);
	int k;

	memset(i, 0, INODE_SIZE);
	put16(i + 0, mode);
	put32(i + 4, size);
	put32(i + 8, MTIME);
	put32(i + 12, MTIME);
	put32(i + 16, MTIME);
	put16(i + 26, (uint32_t)links);
	put32(i + 28, (uint32_t)nblocks * (BS / 512)); /* i_blocks counts 512-byte units */
	for (k = 0; k < nblocks && k < 12; k++)
		put32(i + 40 + k * 4, (uint32_t)blocks[k]);
	if (nblocks > 12) {
		int ind = alloc_block();

		put32(i + 40 + 12 * 4, (uint32_t)ind);
		put32(i + 28, (uint32_t)(nblocks + 1) * (BS / 512));
		for (k = 12; k < nblocks; k++) {
			if (k - 12 >= BS / 4) {
				fprintf(stderr, "mkext2: file too large\n");
				exit(1);
			}
			put32(blockp(ind) + (k - 12) * 4, (uint32_t)blocks[k]);
		}
	}
}

/* Directory entries are packed into blocks; the last one in a block stretches to the end of it. */
struct dirbuf {
	uint8_t blk[64][BS];
	int nblk, used, last_off; /* bytes used in the current block, offset of its last entry */
};

static void dir_add(struct dirbuf *d, int ino, const char *name, int type)
{
	int nlen = (int)strlen(name), need = (8 + nlen + 3) & ~3;

	if (d->nblk == 0 || d->used + need > BS) {
		if (d->nblk) /* close the previous block: its last entry takes the rest */
			put16(d->blk[d->nblk - 1] + d->last_off + 4, (uint32_t)(BS - d->last_off));
		d->nblk++;
		d->used = 0;
	}
	{
		uint8_t *e = d->blk[d->nblk - 1] + d->used;

		put32(e, (uint32_t)ino);
		put16(e + 4, (uint32_t)need);
		e[6] = (uint8_t)nlen;
		e[7] = (uint8_t)type;
		memcpy(e + 8, name, (size_t)nlen);
		d->last_off = d->used;
		d->used += need;
	}
}

static void dir_finish(struct dirbuf *d)
{
	put16(d->blk[d->nblk - 1] + d->last_off + 4, (uint32_t)(BS - d->last_off));
}

static int ndirs = 1; /* for the group descriptor: the root */

static void build_dir(const char *path, int ino, int parent)
{
	struct dirbuf *d = calloc(1, sizeof *d);
	DIR *dp = opendir(path);
	struct dirent *de;
	int blocks[64], i, links = 2;

	dir_add(d, ino, ".", 2);
	dir_add(d, parent, "..", 2);
	while (dp && (de = readdir(dp))) {
		char full[1024];
		struct stat st;
		int child;

		if (de->d_name[0] == '.' || strlen(de->d_name) > 255)
			continue;
		snprintf(full, sizeof full, "%s/%s", path, de->d_name);
		if (stat(full, &st))
			continue;
		if (next_inode > NINODES) {
			fprintf(stderr, "mkext2: out of inodes\n");
			exit(1);
		}
		child = next_inode++;
		if (S_ISDIR(st.st_mode)) {
			ndirs++;
			links++;
			dir_add(d, child, de->d_name, 2);
			build_dir(full, child, ino);
		} else {
			FILE *f = fopen(full, "rb");
			uint8_t *buf = malloc((size_t)st.st_size + 1);
			int nb = (int)((st.st_size + BS - 1) / BS), b[300];

			if (!f || fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
				fprintf(stderr, "mkext2: cannot read %s\n", full);
				exit(1);
			}
			fclose(f);
			if (nb > 12 + BS / 4) {
				fprintf(stderr, "mkext2: %s is too large\n", full);
				exit(1);
			}
			for (i = 0; i < nb; i++) {
				size_t left = (size_t)st.st_size - (size_t)i * BS;

				b[i] = alloc_block();
				memcpy(blockp(b[i]), buf + (size_t)i * BS, left < BS ? left : BS);
			}
			set_inode(child, 0x81A4, (uint32_t)st.st_size, b, nb, 1);
			dir_add(d, child, de->d_name, 1);
			free(buf);
		}
	}
	if (dp)
		closedir(dp);
	dir_finish(d);
	for (i = 0; i < d->nblk; i++) {
		blocks[i] = alloc_block();
		memcpy(blockp(blocks[i]), d->blk[i], BS);
	}
	set_inode(ino, 0x41ED, (uint32_t)d->nblk * BS, blocks, d->nblk, links);
	free(d);
}

int main(int argc, char **argv)
{
	uint8_t *sb = blockp(1), *gd = blockp(2);
	int i, with_mbr = argc == 4 && !strcmp(argv[3], "mbr");
	FILE *out;

	if (argc < 3) {
		fprintf(stderr, "usage: mkext2 out.img rootdir [mbr]\n");
		return 1;
	}
	build_dir(argv[2], 2, 2);

	/* superblock */
	put32(sb + 0, NINODES);
	put32(sb + 4, NBLOCKS);
	put32(sb + 12, NBLOCKS - (uint32_t)next_block);          /* free blocks */
	put32(sb + 16, NINODES - ((uint32_t)next_inode - 1));    /* free inodes */
	put32(sb + 20, 1);                                       /* first data block (1 for 1 KiB blocks) */
	put32(sb + 24, 0);                                       /* log block size: 1024 << 0 */
	put32(sb + 28, 0);
	put32(sb + 32, 8192);                                    /* blocks per group */
	put32(sb + 36, 8192);
	put32(sb + 40, NINODES);                                 /* inodes per group */
	put32(sb + 44, MTIME);
	put32(sb + 48, MTIME);
	put16(sb + 52, 0);
	put16(sb + 54, 20);
	put16(sb + 56, 0xEF53);                                  /* magic */
	put16(sb + 58, 1);                                       /* state: clean */
	put16(sb + 60, 1);
	put32(sb + 76, 1);                                       /* revision 1 */
	put32(sb + 84, 11);                                      /* first non-reserved inode */
	put16(sb + 88, INODE_SIZE);
	memcpy(sb + 120, "tinyext2", 8);                         /* volume name */

	/* one group: bitmaps at blocks 3 and 4, inode table at 5 */
	put32(gd + 0, 3);
	put32(gd + 4, 4);
	put32(gd + 8, 5);
	put16(gd + 12, (uint32_t)(NBLOCKS - next_block));
	put16(gd + 14, (uint32_t)(NINODES - (next_inode - 1)));
	put16(gd + 16, (uint32_t)ndirs);
	for (i = 1; i < next_block; i++) /* block bitmap: bit n = block (first_data_block + n) */
		blockp(3)[(i - 1) / 8] |= (uint8_t)(1u << ((i - 1) % 8));
	for (i = NBLOCKS; i < 8192; i++) /* bits beyond the end of the file system count as used */
		if ((i - 1) / 8 < BS)
			blockp(3)[(i - 1) / 8] |= (uint8_t)(1u << ((i - 1) % 8));
	for (i = 1; i < next_inode; i++)
		blockp(4)[(i - 1) / 8] |= (uint8_t)(1u << ((i - 1) % 8));
	for (i = NINODES + 1; i <= BS * 8; i++)
		blockp(4)[(i - 1) / 8] |= (uint8_t)(1u << ((i - 1) % 8));
	{
		int r;

		for (r = 1; r <= 10; r++) /* reserved inodes */
			blockp(4)[(r - 1) / 8] |= (uint8_t)(1u << ((r - 1) % 8));
	}

	out = fopen(argv[1], "wb");
	if (!out) {
		perror(argv[1]);
		return 1;
	}
	if (with_mbr) {
		uint8_t mbr[512 * 8];
		uint8_t *e = mbr + 446;

		memset(mbr, 0, sizeof mbr);
		e[0] = 0x80;
		e[4] = 0x83;
		put32(e + 8, 8);
		put32(e + 12, NBLOCKS * 2);
		mbr[510] = 0x55;
		mbr[511] = 0xAA;
		fwrite(mbr, 1, sizeof mbr, out);
	}
	fwrite(img, 1, sizeof img, out);
	fclose(out);
	printf("mkext2: %s: %d blocks used of %d, %d inodes\n", argv[1], next_block, NBLOCKS, next_inode - 1);
	return 0;
}
