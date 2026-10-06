#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>
#include "idt.h"

void panic(const char *fmt, ...) __attribute__((noreturn));
void debug_hexdump(const void *addr, uint32_t len);
void debug_dump_regs(const struct regs *r);
void debug_backtrace(uint32_t ebp); /* walk the frame-pointer chain, naming functions from the symbol table */
#define debug_here() debug_backtrace((uint32_t)__builtin_frame_address(0))

/* Failed checks panic with the expression, file, line and function, then a stack trace. */
#define ASSERT(c) 	do { if (!(c)) panic("assertion failed: %s (%s:%d in %s)", #c, __FILE__, __LINE__, __func__); } while (0)
#define ASSERT_MSG(c, ...) 	do { if (!(c)) assert_fail_msg(#c, __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)
#define BUG() panic("BUG at %s:%d in %s", __FILE__, __LINE__, __func__)

void assert_fail_msg(const char *expr, const char *file, int line, const char *func, const char *fmt, ...)
	__attribute__((noreturn, format(printf, 5, 6)));

#endif
