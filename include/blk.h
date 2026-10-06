#ifndef BLK_H
#define BLK_H

#include <stdint.h>

/*
 * Block devices: anything addressed in 512-byte sectors. ATA drives, RAM disks, loopback devices (a file
 * seen as a disk) and partitions all look the same to filesystems such as ext2. See docs/storage.md.
 */
#define BLK_MAX     16
#define BLK_NAME    12

struct blkdev {
	char name[BLK_NAME];
	uint32_t sectors;
	int (*read)(struct blkdev *d, uint32_t lba, uint32_t n, void *buf);        /* 0 on success */
	int (*write)(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf);
	void *priv;
	struct blkdev *parent;   /* for a partition: the device it is carved from */
	uint32_t offset;         /* ... and where it starts */
	int used;
	const char *kind;        /* "ata", "ram", "loop", "part" */
};

void          blk_init(void);                  /* registers the ATA drives */
struct blkdev *blk_find(const char *name);
struct blkdev *blk_get(int index);             /* for listing; NULL for an unused slot */
int           blk_read(struct blkdev *d, uint32_t lba, uint32_t n, void *buf);   /* checks the range */
int           blk_write(struct blkdev *d, uint32_t lba, uint32_t n, const void *buf);
int           blk_remove(const char *name);    /* frees a RAM or loop device (and its partitions) */

int  ramdisk_create(const char *name, uint32_t kib);   /* 0 or FS_E* */
int  loop_attach(const char *name, const char *path);  /* the file contents become the device */
int  loop_sync(const char *name);                      /* write a loop device back to its file */

/* Partition table (MBR): registers NAMEp1.. for every used entry; returns how many, or a negative error. */
struct part_entry {
	uint8_t boot, type;
	uint32_t start, sectors;
};
int  part_read_table(struct blkdev *d, struct part_entry out[4]);   /* number of used entries, <0 if no MBR */
int  part_scan(struct blkdev *d);
int  part_write_demo(struct blkdev *d);                             /* writes a two-partition MBR (for tests) */


/* shell commands (kernel/blkcmd.c) */
int cmd_blkdev(int argc, char **argv);
int cmd_ramdisk(int argc, char **argv);
int cmd_losetup(int argc, char **argv);
int cmd_fdisk(int argc, char **argv);

#endif
