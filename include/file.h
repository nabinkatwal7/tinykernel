#ifndef FILE_H
#define FILE_H

#include <stdint.h>

/* open() flags (values follow the usual Unix ones) */
#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREAT  0x040
#define O_TRUNC  0x200
#define O_APPEND 0x400

#define FILE_FD_MAX   20    /* descriptors per program */
#define FILE_MAX_OPEN 16    /* open regular files at once */
#define FILE_FIRST_FD 3     /* 0 = stdin (keyboard), 1 = stdout, 2 = stderr (console) */
#define FILE_MAX_SIZE (256 * 1024)

/*
 * Open files of the running program. Because TinyFS stores each file as one contiguous extent,
 * an open file is buffered in kernel memory: it is loaded at open() and, if it was opened for
 * writing, stored back with fs_write() at close(). All results are >= 0 or a negative FS_E* code.
 */
int file_open(const char *path, int flags);   /* lowest free descriptor */
int file_dup(int fd);                          /* new descriptor sharing the same open file */
int file_dup2(int fd, int target);             /* make target refer to fd's open file (closing target) */
void file_reset(void);                         /* fresh table: only 0, 1, 2 open */
int file_close(int fd);
int file_read(int fd, void *buf, uint32_t n);
int file_write(int fd, const void *buf, uint32_t n);
void file_close_all(void);   /* called when a program ends */
int file_open_count(void);

#endif
