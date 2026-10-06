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
#define SYS_MMAP    31
#define SYS_MUNMAP  32
#define SYS_MSYNC   33
#define SYS_SHMGET  28
#define SYS_SHMAT   29
#define SYS_SHMDT   30
#define SYS_FORK    26
#define SYS_WAITPID 27
#define SYS_NANOSLEEP 25
#define SYS_CLOCK   24
#define SYS_LSEEK   23
#define SYS_GETCWD  21
#define SYS_CHDIR   22
#define SYS_MKDIR   19
#define SYS_RMDIR   20
#define SYS_DUP     17
#define SYS_DUP2    18
#define SYS_EXEC    14
#define SYS_SPAWN   15
#define SYS_KCMD    16
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
/* Goes through descriptor 1, so redirections apply (SYS_PUTCHAR bypasses the fd table). */
static inline int  putchar(int c)                   { char ch = (char)c; return write(1, &ch, 1); }
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
static inline int  mkdir(const char *path)         { return syscall3(SYS_MKDIR, (int)path, 0, 0); }
static inline int  rmdir(const char *path)         { return syscall3(SYS_RMDIR, (int)path, 0, 0); }
static inline int  getcwd(char *buf, int size)    { return syscall3(SYS_GETCWD, (int)buf, size, 0); }
static inline int  chdir(const char *path)         { return syscall3(SYS_CHDIR, (int)path, 0, 0); }
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
static inline int  lseek(int fd, int off, int whence) { return syscall3(SYS_LSEEK, fd, off, whence); }
static inline int  dup(int fd)                      { return syscall3(SYS_DUP, fd, 0, 0); }
static inline int  dup2(int fd, int target)         { return syscall3(SYS_DUP2, fd, target, 0); }
static inline int  close(int fd)                     { return syscall3(SYS_CLOSE, fd, 0, 0); }
static inline void *sbrk(int delta)                 { return (void *)syscall3(SYS_SBRK, delta, 0, 0); }
/* Replaces this program with another; only returns (-1) if it could not be started. */
static inline int  exec(const char *path, char *const argv[]) { return syscall3(SYS_EXEC, (int)path, (int)argv, 0); }
/* Runs a program to completion and returns its exit code (-1 if it does not exist). */
static inline int  spawn(const char *path, char *const argv[]) { return syscall3(SYS_SPAWN, (int)path, (int)argv, 0); }
/* Runs a kernel shell command ('ls', 'ps', ...) and returns its status. */
static inline int  kcmd(const char *line)            { return syscall3(SYS_KCMD, (int)line, 0, 0); }
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
struct timespec { unsigned tv_sec, tv_nsec; };
static inline int  clock_gettime(int id, struct timespec *ts) { return syscall3(SYS_CLOCK, id, (int)ts, 0); }
static inline int  nanosleep(unsigned sec, unsigned nsec) { return syscall3(SYS_NANOSLEEP, (int)sec, (int)nsec, 0); }
static inline void usleep(unsigned us)               { nanosleep(us / 1000000, (us % 1000000) * 1000); }
/* fork(): 0 in the child, the child's pid in the parent, -1 on failure. waitpid() blocks until that child exits. */
static inline int  fork(void)                      { return syscall3(SYS_FORK, 0, 0, 0); }
static inline int  waitpid(int pid, int *status)   { return syscall3(SYS_WAITPID, pid, (int)status, 0); }
/* Shared memory: processes using the same key see the same pages. */
static inline int   shmget(int key, unsigned size) { return syscall3(SYS_SHMGET, key, (int)size, 0); }
static inline void *shmat(int id)                  { return (void *)syscall3(SYS_SHMAT, id, 0, 0); }
static inline int   shmdt(void *addr)              { return syscall3(SYS_SHMDT, (int)addr, 0, 0); }
#define MAP_PRIVATE 0
#define MAP_SHARED  1
/* Map a file (length 0 = all of it). Shared mappings are written back by msync/munmap. 0 on failure. */
static inline void *mmap(const char *path, unsigned length, int flags) { return (void *)syscall3(SYS_MMAP, (int)path, (int)length, flags); }
static inline int   munmap(void *addr)             { return syscall3(SYS_MUNMAP, (int)addr, 0, 0); }
static inline int   msync(void *addr)              { return syscall3(SYS_MSYNC, (int)addr, 0, 0); }
static inline int  getpid(void)                    { return syscall3(SYS_GETPID, 0, 0, 0); }
static inline int  getppid(void)                   { return syscall3(SYS_GETPPID, 0, 0, 0); }
static inline void sleep_ms(int ms)                 { syscall3(SYS_SLEEP, ms, 0, 0); }

#endif
