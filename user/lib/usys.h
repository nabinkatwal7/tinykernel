#ifndef USYS_H
#define USYS_H

/* Tiny user-space runtime: system call stubs for the kernel's int 0x80 ABI. */

#define SYS_EXIT     1
#define SYS_WRITE    2
#define SYS_PUTCHAR  3
#define SYS_TICKS    4
#define SYS_GETKEY   5
#define SYS_SLEEP    6
#define SYS_OPEN     7
#define SYS_CLOSE    8
#define SYS_READ     9
#define SYS_FWRITE  10
#define SYS_EXEC    14
#define SYS_SBRK    13
#define SYS_GETPID  11
#define SYS_GETPPID 12

static inline int syscall3(int num, int a, int b, int c)
{
	int ret;

	__asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
	return ret;
}

void exit(int code) __attribute__((noreturn));

/* write(fd, buf, n): fd 1 and 2 print on the console, others are files from open() */
static inline int  write(int fd, const void *buf, int len) { return syscall3(SYS_FWRITE, fd, (int)buf, len); }
static inline int  read(int fd, void *buf, int len)        { return syscall3(SYS_READ, fd, (int)buf, len); }
static inline int  putchar(int c)                   { return syscall3(SYS_PUTCHAR, c, 0, 0); }
static inline int  ticks(void)                      { return syscall3(SYS_TICKS, 0, 0, 0); }
static inline int  getkey(void)                     { return syscall3(SYS_GETKEY, 0, 0, 0); }
/* open() flags and error codes match the kernel's file.h / fs.h */
#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREAT  0x040
#define O_TRUNC  0x200
#define O_APPEND 0x400

static inline int  open(const char *path, int flags) { return syscall3(SYS_OPEN, (int)path, flags, 0); }
static inline int  close(int fd)                     { return syscall3(SYS_CLOSE, fd, 0, 0); }
static inline void *sbrk(int delta)                 { return (void *)syscall3(SYS_SBRK, delta, 0, 0); }
/* Replaces this program with another; only returns (-1) if it could not be started. */
static inline int  exec(const char *path, char *const argv[]) { return syscall3(SYS_EXEC, (int)path, (int)argv, 0); }
static inline int  getpid(void)                    { return syscall3(SYS_GETPID, 0, 0, 0); }
static inline int  getppid(void)                   { return syscall3(SYS_GETPPID, 0, 0, 0); }
static inline void sleep_ms(int ms)                 { syscall3(SYS_SLEEP, ms, 0, 0); }

#endif
