#ifndef SYSCALL_H
#define SYSCALL_H

#include "idt.h"

/* int 0x80 ABI: eax = number, ebx/ecx = arguments, result returned in eax. */
#define SYS_EXIT     1 /* ebx = exit code */
#define SYS_WRITE    2 /* ebx = pointer, ecx = length */
#define SYS_PUTCHAR  3 /* ebx = char */
#define SYS_TICKS    4 /* returns timer ticks */
#define SYS_GETKEY   5 /* blocks, returns a key code */
#define SYS_SLEEP    6 /* ebx = milliseconds */
#define SYS_OPEN     7 /* ebx = path, ecx = flags; returns fd or a negative error */
#define SYS_CLOSE    8 /* ebx = fd */
#define SYS_READ     9 /* ebx = fd, ecx = buffer, edx = count; returns bytes, 0 at EOF, or < 0 */
#define SYS_GETPID  11 /* id of the task the program runs in */
#define SYS_GETPPID 12
#define SYS_SBRK    13 /* ebx = delta; returns the old program break or -1 */
#define SYS_EXEC    14 /* ebx = path, ecx = NULL-terminated argv (or 0); does not return on success */
#define SYS_SPAWN   15 /* like exec, but runs the program to completion and returns its exit code */
#define SYS_KCMD    16 /* ebx = command line for the kernel shell; returns its status */
#define SYS_DUP     17 /* ebx = fd; returns a new descriptor */
#define SYS_DUP2    18 /* ebx = fd, ecx = target */
#define SYS_FWRITE  10 /* ebx = fd, ecx = buffer, edx = count; fd 1 and 2 are the console */

void syscall_dispatch(struct regs *r);

#endif
