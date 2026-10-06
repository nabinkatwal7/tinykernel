#include "stdlib.h"
#include "string.h"
#include "usys.h"

/* Every block (used or free) starts with a header; blocks are kept in address order. */
struct hdr {
	size_t size;       /* payload bytes, multiple of 8 */
	int free;
	struct hdr *next;  /* next block in address order */
};

#define HDR ((sizeof(struct hdr) + 7) & ~(size_t)7)

static struct hdr *head, *tail;

static struct hdr *grow(size_t n)
{
	size_t total = HDR + n;
	struct hdr *h = sbrk((int)total);

	if (h == (struct hdr *)-1)
		return 0;
	h->size = n;
	h->free = 1;
	h->next = 0;
	if (tail) {
		tail->next = h;
		if (tail->free && (char *)tail + HDR + tail->size == (char *)h) { /* merge with a free tail */
			tail->size += HDR + h->size;
			tail->next = 0;
			return tail;
		}
	} else {
		head = h;
	}
	tail = h;
	return h;
}

void *malloc(size_t n)
{
	struct hdr *h;

	if (n == 0)
		return 0;
	n = (n + 7) & ~(size_t)7;
	for (h = head; h; h = h->next)
		if (h->free && h->size >= n)
			break;
	if (!h) {
		h = grow(n > 4096 ? n : 4096);
		if (!h || h->size < n)
			return 0;
	}
	if (h->size >= n + HDR + 8) { /* split off the remainder */
		struct hdr *rest = (struct hdr *)((char *)h + HDR + n);

		rest->size = h->size - n - HDR;
		rest->free = 1;
		rest->next = h->next;
		h->next = rest;
		h->size = n;
		if (tail == h)
			tail = rest;
	}
	h->free = 0;
	return (char *)h + HDR;
}

void free(void *p)
{
	struct hdr *h, *it;

	if (!p)
		return;
	h = (struct hdr *)((char *)p - HDR);
	h->free = 1;
	for (it = head; it; it = it->next) { /* coalesce every adjacent free pair */
		while (it->free && it->next && it->next->free
		       && (char *)it + HDR + it->size == (char *)it->next) {
			if (tail == it->next)
				tail = it;
			it->size += HDR + it->next->size;
			it->next = it->next->next;
		}
	}
}

void *calloc(size_t n, size_t size)
{
	void *p = malloc(n * size);

	if (p)
		memset(p, 0, n * size);
	return p;
}

void *realloc(void *p, size_t n)
{
	struct hdr *h;
	void *q;

	if (!p)
		return malloc(n);
	h = (struct hdr *)((char *)p - HDR);
	if (h->size >= n)
		return p;
	q = malloc(n);
	if (q) {
		memcpy(q, p, h->size);
		free(p);
	}
	return q;
}
