#ifndef USTDIO_H
#define USTDIO_H

#include <stdarg.h>
#include <stddef.h>

/* Formats: %c %s %d %u %x %X %p %% with '-' and '0' flags and a width. */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int puts(const char *s);   /* writes s and a newline */

#endif
