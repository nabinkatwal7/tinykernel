#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>

/* FAT12 driver (the format of 720 KB / 1.44 MB floppies), on one ATA drive. 8.3 names only (no long file names). */
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
int  fat12_write(const char *path, const void *data, uint32_t size); /* create or replace a file */
int  fat12_delete(const char *path);                                 /* files only */
int  fat12_mkdir(const char *path);
int  fat12_rmdir(const char *path);                                  /* must be empty */
int  fat12_rename(const char *from, const char *to);                 /* files only; may move between directories */
void fat12_info(uint32_t *total_sectors, uint32_t *clusters, uint32_t *cluster_bytes);

#endif
