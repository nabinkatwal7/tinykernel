#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>

/* Read-only FAT12 driver (the format of 720 KB / 1.44 MB floppies), on one ATA drive. */
struct fat12_entry {
	char name[13];     /* "NAME.EXT" as stored (upper case), NUL-terminated */
	uint32_t size;
	int is_dir;
};

int  fat12_mount(int ata_dev);      /* 0, or a negative FS_E* code */
int  fat12_mounted(void);
int  fat12_stat(const char *path, struct fat12_entry *out);   /* FS_ENOENT etc. */
int  fat12_read(const char *path, void *buf, uint32_t cap);   /* bytes read, or FS_ETOOBIG ... */
int  fat12_list(const char *path, struct fat12_entry *out, int max); /* entries in a directory */
void fat12_info(uint32_t *total_sectors, uint32_t *clusters, uint32_t *cluster_bytes);

#endif
