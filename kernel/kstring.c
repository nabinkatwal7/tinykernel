#include "kstring.h"

size_t kstrlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

int kstrcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int kstrncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}
	return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

void kstrlcpy(char *dst, const char *src, size_t n)
{
	size_t i;

	for (i = 0; i + 1 < n && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

int kstrtoul(const char *s, uint32_t *out)
{
	uint32_t v = 0, base = 10;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	if (!*s)
		return -1;
	for (; *s; s++) {
		uint32_t d;

		if (*s >= '0' && *s <= '9')
			d = (uint32_t)(*s - '0');
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = (uint32_t)(*s - 'a' + 10);
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = (uint32_t)(*s - 'A' + 10);
		else
			return -1;
		v = v * base + d;
	}
	*out = v;
	return 0;
}

void *memset(void *d, int c, size_t n)
{
	unsigned char *p = d;

	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;

	if (p < q) {
		while (n--)
			*p++ = *q++;
	} else {
		p += n;
		q += n;
		while (n--)
			*--p = *--q;
	}
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;

	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p - *q;
	return 0;
}
