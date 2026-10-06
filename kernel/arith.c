#include "arith.h"

#include "env.h"
#include "kstring.h"

struct parser {
	const char *p;
	int err;
};

static int32_t expr_or(struct parser *ps);

static void skip(struct parser *ps)
{
	while (*ps->p == ' ' || *ps->p == '\t')
		ps->p++;
}

static int is_name_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int32_t number_of(const char *s)
{
	uint32_t v = 0;
	int neg = 0;

	if (*s == '-') {
		neg = 1;
		s++;
	}
	if (!kstrtoul(s, &v))
		return neg ? -(int32_t)v : (int32_t)v;
	return 0;
}

static int32_t primary(struct parser *ps)
{
	int32_t v = 0;

	skip(ps);
	if (*ps->p == '(') {
		ps->p++;
		v = expr_or(ps);
		skip(ps);
		if (*ps->p != ')')
			ps->err = 1;
		else
			ps->p++;
		return v;
	}
	if (*ps->p >= '0' && *ps->p <= '9') {
		uint32_t n = 0;
		int hex = ps->p[0] == '0' && (ps->p[1] == 'x' || ps->p[1] == 'X');

		if (hex)
			ps->p += 2;
		for (;;) {
			char c = *ps->p;
			int d;

			if (c >= '0' && c <= '9')
				d = c - '0';
			else if (hex && c >= 'a' && c <= 'f')
				d = c - 'a' + 10;
			else if (hex && c >= 'A' && c <= 'F')
				d = c - 'A' + 10;
			else
				break;
			n = n * (hex ? 16 : 10) + (uint32_t)d;
			ps->p++;
		}
		return (int32_t)n;
	}
	if (is_name_start(*ps->p)) {
		char name[ENV_NAME_MAX];
		int n = 0;
		const char *val;

		while (is_name_start(*ps->p) || (*ps->p >= '0' && *ps->p <= '9')) {
			if (n < ENV_NAME_MAX - 1)
				name[n++] = *ps->p;
			ps->p++;
		}
		name[n] = '\0';
		val = env_get(name);
		return val ? number_of(val) : 0;
	}
	ps->err = 1;
	return 0;
}

static int32_t unary(struct parser *ps)
{
	skip(ps);
	if (*ps->p == '-') {
		ps->p++;
		return -unary(ps);
	}
	if (*ps->p == '!') {
		ps->p++;
		return !unary(ps);
	}
	if (*ps->p == '+') {
		ps->p++;
		return unary(ps);
	}
	return primary(ps);
}

static int32_t term(struct parser *ps)
{
	int32_t v = unary(ps);

	for (;;) {
		char op;
		int32_t r;

		skip(ps);
		op = *ps->p;
		if (op != '*' && op != '/' && op != '%')
			return v;
		ps->p++;
		r = unary(ps);
		if (op == '*') {
			v *= r;
		} else if (!r) {
			ps->err = 1; /* division by zero */
		} else if (r == -1) { /* x / -1 and x % -1 without the INT_MIN overflow trap */
			v = op == '/' ? -v : 0;
		} else {
			v = op == '/' ? v / r : v % r;
		}
	}
}

static int32_t sum(struct parser *ps)
{
	int32_t v = term(ps);

	for (;;) {
		char op;

		skip(ps);
		op = *ps->p;
		if (op != '+' && op != '-')
			return v;
		ps->p++;
		v = op == '+' ? v + term(ps) : v - term(ps);
	}
}

static int32_t relation(struct parser *ps)
{
	int32_t v = sum(ps);

	for (;;) {
		skip(ps);
		if (ps->p[0] == '<' && ps->p[1] == '=') {
			ps->p += 2;
			v = v <= sum(ps);
		} else if (ps->p[0] == '>' && ps->p[1] == '=') {
			ps->p += 2;
			v = v >= sum(ps);
		} else if (ps->p[0] == '<') {
			ps->p++;
			v = v < sum(ps);
		} else if (ps->p[0] == '>') {
			ps->p++;
			v = v > sum(ps);
		} else {
			return v;
		}
	}
}

static int32_t equality(struct parser *ps)
{
	int32_t v = relation(ps);

	for (;;) {
		skip(ps);
		if (ps->p[0] == '=' && ps->p[1] == '=') {
			ps->p += 2;
			v = v == relation(ps);
		} else if (ps->p[0] == '!' && ps->p[1] == '=') {
			ps->p += 2;
			v = v != relation(ps);
		} else {
			return v;
		}
	}
}

static int32_t expr_and(struct parser *ps)
{
	int32_t v = equality(ps);

	for (;;) {
		skip(ps);
		if (ps->p[0] != '&' || ps->p[1] != '&')
			return v;
		ps->p += 2;
		v = equality(ps) && v;
	}
}

static int32_t expr_or(struct parser *ps)
{
	int32_t v = expr_and(ps);

	for (;;) {
		skip(ps);
		if (ps->p[0] != '|' || ps->p[1] != '|')
			return v;
		ps->p += 2;
		v = expr_and(ps) || v;
	}
}

int arith_eval(const char *expr, int32_t *result)
{
	struct parser ps = { expr, 0 };
	int32_t v = expr_or(&ps);

	skip(&ps);
	if (ps.err || *ps.p)
		return -1;
	*result = v;
	return 0;
}
