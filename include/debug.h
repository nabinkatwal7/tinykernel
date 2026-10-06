#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>
#include "idt.h"

void panic(const char *fmt, ...) __attribute__((noreturn));
void debug_hexdump(const void *addr, uint32_t len);
void debug_dump_regs(const struct regs *r);
void debug_backtrace(uint32_t ebp); /* walk the frame-pointer chain, naming functions from the symbol table */
#define debug_here() debug_backtrace((uint32_t)__builtin_frame_address(0))

#define ASSERT(c) \
	do { if (!(c)) panic("assertion failed: %s (%s:%d)", #c, __FILE__, __LINE__); } while (0)

#endif
