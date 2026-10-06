#include "string.h"

size_t strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}
	return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *strcpy(char *dst, const char *src)
{
	char *d = dst;

	while ((*d++ = *src++))
		;
	return dst;
}

/* Like the standard one: pads with NULs, and does not terminate if src is n bytes or longer. */
char *strncpy(char *dst, const char *src, size_t n)
{
	size_t i;

	for (i = 0; i < n && src[i]; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = '\0';
	return dst;
}

char *strcat(char *dst, const char *src)
{
	strcpy(dst + strlen(dst), src);
	return dst;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return 0;
	}
}

char *strrchr(const char *s, int c)
{
	const char *last = 0;

	for (;; s++) {
		if (*s == (char)c)
			last = s;
		if (!*s)
			return (char *)last;
	}
}

char *strstr(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	if (!n)
		return (char *)hay;
	for (; *hay; hay++)
		if (!strncmp(hay, needle, n))
			return (char *)hay;
	return 0;
}

void *memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;

	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;

	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}

int atoi(const char *s)
{
	int sign = 1, v = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '-' || *s == '+')
		sign = *s++ == '-' ? -1 : 1;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return sign * v;
}
