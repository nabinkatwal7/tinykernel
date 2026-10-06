#ifndef USYS_H
#define USYS_H

/* Tiny user-space runtime: system call stubs for the kernel's int 0x80 ABI. */

#define SYS_EXIT     1
#define SYS_WRITE    2
#define SYS_PUTCHAR  3
#define SYS_TICKS    4
#define SYS_GETKEY   5
#define SYS_SLEEP    6

static inline int syscall3(int num, int a, int b, int c)
{
	int ret;

	(void)c;
	__asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(a), "c"(b) : "memory");
	return ret;
}

void exit(int code) __attribute__((noreturn));

static inline int  write(const char *buf, int len)  { return syscall3(SYS_WRITE, (int)buf, len, 0); }
static inline int  putchar(int c)                   { return syscall3(SYS_PUTCHAR, c, 0, 0); }
static inline int  ticks(void)                      { return syscall3(SYS_TICKS, 0, 0, 0); }
static inline int  getkey(void)                     { return syscall3(SYS_GETKEY, 0, 0, 0); }
static inline void sleep_ms(int ms)                 { syscall3(SYS_SLEEP, ms, 0, 0); }

#endif
