#ifndef KSTRING_H
#define KSTRING_H

#include <stddef.h>
#include <stdint.h>

size_t kstrlen(const char *s);
int    kstrcmp(const char *a, const char *b);
int    kstrncmp(const char *a, const char *b, size_t n);
/* Copies at most n-1 chars and always NUL-terminates (n > 0). */
void   kstrlcpy(char *dst, const char *src, size_t n);
int    kstrtoul(const char *s, uint32_t *out); /* decimal or 0x hex; 0 on success */

/* Exported under libc names too: the compiler may emit calls to these. */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

#endif
