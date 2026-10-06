#include "stdio.h"
#include "string.h"
#include "usys.h"

struct sink {
	void (*put)(struct sink *s, char c);
	char *buf;       /* snprintf target */
	size_t cap, len; /* snprintf: capacity and chars produced (even if truncated) */
	char tmp[64];    /* printf: staging buffer, flushed with one write() */
	int used;
};

static void mem_put(struct sink *s, char c)
{
	if (s->len + 1 < s->cap)
		s->buf[s->len] = c;
	s->len++;
}

static void flush(struct sink *s)
{
	if (s->used) {
		write(1, s->tmp, s->used);
		s->used = 0;
	}
}

static void out_put(struct sink *s, char c)
{
	if (s->used == (int)sizeof s->tmp)
		flush(s);
	s->tmp[s->used++] = c;
	s->len++;
}

static void pad(struct sink *s, int n, char c)
{
	while (n-- > 0)
		s->put(s, c);
}

static void emit_num(struct sink *s, unsigned v, unsigned base, int neg, int upper, int width,
		     int left, int zero)
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
		pad(s, width - len, ' ');
	if (neg)
		s->put(s, '-');
	if (!left && zero)
		pad(s, width - len, '0');
	while (n--)
		s->put(s, buf[n]);
	if (left)
		pad(s, width - len, ' ');
}

static void format(struct sink *s, const char *fmt, va_list ap)
{
	char c;

	while ((c = *fmt++)) {
		int left = 0, zero = 0, width = 0;

		if (c != '%') {
			s->put(s, c);
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
		c = *fmt++;
		switch (c) {
		case 'c':
			s->put(s, (char)va_arg(ap, int));
			break;
		case 's': {
			const char *str = va_arg(ap, const char *);
			int len;

			if (!str)
				str = "(null)";
			len = (int)strlen(str);
			if (!left)
				pad(s, width - len, ' ');
			while (*str)
				s->put(s, *str++);
			if (left)
				pad(s, width - len, ' ');
			break;
		}
		case 'd': {
			int v = va_arg(ap, int);

			emit_num(s, v < 0 ? 0u - (unsigned)v : (unsigned)v, 10, v < 0, 0, width, left, zero);
			break;
		}
		case 'u':
			emit_num(s, va_arg(ap, unsigned), 10, 0, 0, width, left, zero);
			break;
		case 'x':
		case 'X':
			emit_num(s, va_arg(ap, unsigned), 16, 0, c == 'X', width, left, zero);
			break;
		case 'p':
			s->put(s, '0');
			s->put(s, 'x');
			emit_num(s, (unsigned)va_arg(ap, void *), 16, 0, 0, 8, 0, 1);
			break;
		case '%':
			s->put(s, '%');
			break;
		case '\0':
			return;
		default:
			s->put(s, '%');
			s->put(s, c);
			break;
		}
	}
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct sink s;

	s.put = mem_put;
	s.buf = buf;
	s.cap = size;
	s.len = 0;
	format(&s, fmt, ap);
	if (size)
		buf[s.len < size ? s.len : size - 1] = '\0';
	return (int)s.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

int vprintf(const char *fmt, va_list ap)
{
	struct sink s;

	s.put = out_put;
	s.len = 0;
	s.used = 0;
	format(&s, fmt, ap);
	flush(&s);
	return (int)s.len;
}

int printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vprintf(fmt, ap);
	va_end(ap);
	return n;
}

int puts(const char *s)
{
	write(1, s, (int)strlen(s));
	putchar('\n');
	return 0;
}
