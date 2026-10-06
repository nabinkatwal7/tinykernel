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

void syscall_dispatch(struct regs *r);

#endif
