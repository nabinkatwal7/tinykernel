#include "kprintf.h"

#include <stdint.h>

struct snctx {
	char *buf;
	size_t cap;
	size_t len;
};

static void sn_put(char c, void *p)
{
	struct snctx *s = p;

	if (s->len + 1 < s->cap)
		s->buf[s->len] = c;
	s->len++;
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
{
	struct snctx s = { buf, cap, 0 };
	va_list ap;

	va_start(ap, fmt);
	kvformat(sn_put, &s, fmt, ap);
	va_end(ap);
	if (cap)
		buf[s.len < cap ? s.len : cap - 1] = '\0';
	return (int)s.len;
}

struct out {
	void (*put)(char, void *);
	void *ctx;
	int n;
};

static void emit(struct out *o, char c)
{
	o->put(c, o->ctx);
	o->n++;
}

static void pad(struct out *o, int count, char c)
{
	while (count-- > 0)
		emit(o, c);
}

static void fmt_str(struct out *o, const char *s, int width, int left)
{
	int len = 0;

	while (s[len])
		len++;
	if (!left)
		pad(o, width - len, ' ');
	while (*s)
		emit(o, *s++);
	if (left)
		pad(o, width - len, ' ');
}

static void fmt_num(struct out *o, uint32_t v, unsigned base, int neg, int upper,
		    int width, int left, int zero)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char buf[12];
	int n = 0, len;

	do {
		buf[n++] = digits[v % base];
		v /= base;
	} while (v);
	len = n + neg;

	if (!left && !zero)
		pad(o, width - len, ' ');
	if (neg)
		emit(o, '-');
	if (!left && zero)
		pad(o, width - len, '0');
	while (n--)
		emit(o, buf[n]);
	if (left)
		pad(o, width - len, ' ');
}

int kvformat(void (*put)(char, void *), void *ctx, const char *fmt, va_list ap)
{
	struct out o = { put, ctx, 0 };
	char c;

	while ((c = *fmt++) != '\0') {
		int left = 0, zero = 0, width = 0;

		if (c != '%') {
			emit(&o, c);
			continue;
		}
		for (;; fmt++) {
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				zero = 1;
			else
				break;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		if (*fmt == 'l')
			fmt++;

		c = *fmt++;
		switch (c) {
		case 'c':
			emit(&o, (char)va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);

			fmt_str(&o, s ? s : "(null)", width, left);
			break;
		}
		case 'd': {
			int v = va_arg(ap, int);

			fmt_num(&o, v < 0 ? 0u - (uint32_t)v : (uint32_t)v, 10, v < 0, 0,
				width, left, zero);
			break;
		}
		case 'u':
			fmt_num(&o, va_arg(ap, unsigned int), 10, 0, 0, width, left, zero);
			break;
		case 'x':
		case 'X':
			fmt_num(&o, va_arg(ap, unsigned int), 16, 0, c == 'X', width, left, zero);
			break;
		case 'p':
			emit(&o, '0');
			emit(&o, 'x');
			fmt_num(&o, (uint32_t)va_arg(ap, void *), 16, 0, 0, 8, 0, 1);
			break;
		case '%':
			emit(&o, '%');
			break;
		case '\0':
			return o.n;
		default:
			emit(&o, '%');
			emit(&o, c);
			break;
		}
	}
	return o.n;
}
