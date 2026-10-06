#ifndef USTDLIB_H
#define USTDLIB_H

#include <stddef.h>

/* First-fit free-list allocator on top of sbrk(). free() coalesces neighbours. */
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);

int   atoi(const char *s);
void  exit(int code) __attribute__((noreturn));

#endif
