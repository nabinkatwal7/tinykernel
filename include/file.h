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
#define FILE_PATH_MAX 64    /* longest path open() accepts, including the NUL */

/*
 * Open files of the running program. Because TinyFS stores each file as one contiguous extent,
 * an open file is buffered in kernel memory: it is loaded at open() and, if it was opened for
 * writing, stored back with fs_write() at close(). All results are >= 0 or a negative FS_E* code.
 */
int file_open(const char *path, int flags);   /* lowest free descriptor */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
int file_seek(int fd, int32_t off, int whence); /* new position or a negative error; regular files only */
/* Advisory file locks (flock): shared locks coexist, an exclusive lock excludes everything else on the same file.
   A lock belongs to the open file and ends with the last close. Without LOCK_NB a conflicting request blocks. */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
int file_flock(int fd, int op);                /* 0, FS_EBUSY (LOCK_NB only) or FS_EINVAL */
/* Sockets (see sock.h): a socket is a descriptor, read/write/close/dup work on it */
struct sock;
int file_socket(int type);                     /* SOCK_STREAM or SOCK_DGRAM; a new descriptor or FS_E* */
struct sock *file_get_sock(int fd);            /* NULL if fd is not a socket */
int file_accept(int fd);                       /* waits for a client on a listening socket; the connection's descriptor */
int file_pipe(int fds[2]);                     /* a pipe: fds[0] reads what is written to fds[1]; 0 or FS_E* */
int file_dup(int fd);                          /* new descriptor sharing the same open file */
int file_dup2(int fd, int target);             /* make target refer to fd's open file (closing target) */
/* While set, descriptor 0 reads from this memory (end of file at its end) instead of the keyboard: pipelines and "<". */
void file_stdin_set(const char *data, uint32_t len);
void file_stdin_clear(void);
int  file_stdin_active(void);
int  console_stdin_read(void *buf, uint32_t n); /* cooked, line-buffered keyboard input with echo */
void file_reset(void);                         /* fresh table: only 0, 1, 2 open */
int file_close(int fd);
int file_read(int fd, void *buf, uint32_t n);
int file_write(int fd, const void *buf, uint32_t n);
void file_close_all(void);   /* called when a program ends */
int file_open_count(void);

#endif
