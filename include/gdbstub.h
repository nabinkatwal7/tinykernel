#ifndef GDBSTUB_H
#define GDBSTUB_H

#include "idt.h"

/*
 * A small GDB remote-protocol stub on COM2 (polled, 115200 8N1). The 'gdbstub' shell command arms it and
 * executes int3; from then on breakpoint (3) and single-step (1) traps in kernel code stop in the stub.
 * Supports: ? g G m M c s k D H qSupported qAttached. Software breakpoints work through memory writes.
 *
 *   qemu-system-i386 ... -serial stdio -serial tcp::1234,server=on,wait=off
 *   (gdb) target remote :1234
 */
int gdbstub_active(void);
void gdbstub_trap(struct regs *r);   /* called from the exception path for vectors 1 and 3 */
void gdbstub_arm(void);              /* enable and break into the debugger now */

#endif
