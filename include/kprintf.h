#ifndef KPRINTF_H
#define KPRINTF_H

#include <stdarg.h>
#include <stddef.h>

/* Core formatter: %c %s %d %u %x %X %p %% with '-' / '0' flags and a width. */
int kvformat(void (*put)(char c, void *ctx), void *ctx, const char *fmt, va_list ap);
int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t cap, const char *fmt, ...);

#endif
