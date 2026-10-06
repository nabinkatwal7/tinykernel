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

#define FILE_MAX_OPEN 16
#define FILE_FIRST_FD 3     /* 0, 1, 2 are the standard streams */
#define FILE_MAX_SIZE (256 * 1024)

/*
 * Open files of the running program. Because TinyFS stores each file as one contiguous extent,
 * an open file is buffered in kernel memory: it is loaded at open() and, if it was opened for
 * writing, stored back with fs_write() at close(). All results are >= 0 or a negative FS_E* code.
 */
int file_open(const char *path, int flags);
int file_close(int fd);
int file_read(int fd, void *buf, uint32_t n);
int file_write(int fd, const void *buf, uint32_t n);
void file_close_all(void);   /* called when a program ends */
int file_open_count(void);

#endif
