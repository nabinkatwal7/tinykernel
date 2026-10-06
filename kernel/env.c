#include "env.h"

#include "kprintf.h"
#include "kstring.h"

static struct {
	char name[ENV_NAME_MAX];
	char value[ENV_VAL_MAX];
	int used;
} vars[ENV_MAX];

static int valid_name(const char *n)
{
	size_t i, len = kstrlen(n);

	if (len == 0 || len >= ENV_NAME_MAX)
		return 0;
	for (i = 0; i < len; i++) {
		char c = n[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'
		      || (i && c >= '0' && c <= '9')))
			return 0;
	}
	return 1;
}

static int find(const char *name)
{
	int i;

	for (i = 0; i < ENV_MAX; i++)
		if (vars[i].used && !kstrcmp(vars[i].name, name))
			return i;
	return -1;
}

int env_set(const char *name, const char *value)
{
	int i = find(name);

	if (!valid_name(name))
		return -1;
	if (i < 0) {
		for (i = 0; i < ENV_MAX && vars[i].used; i++)
			;
		if (i == ENV_MAX)
			return -1;
		kstrlcpy(vars[i].name, name, sizeof vars[i].name);
		vars[i].used = 1;
	}
	kstrlcpy(vars[i].value, value, sizeof vars[i].value);
	return 0;
}

const char *env_get(const char *name)
{
	int i = find(name);

	return i < 0 ? 0 : vars[i].value;
}

int env_unset(const char *name)
{
	int i = find(name);

	if (i < 0)
		return -1;
	vars[i].used = 0;
	return 0;
}

int env_count(void)
{
	int i, n = 0;

	for (i = 0; i < ENV_MAX; i++)
		n += vars[i].used;
	return n;
}

int env_entry(int idx, char *buf, int size)
{
	int i;

	for (i = 0; i < ENV_MAX; i++) {
		if (vars[i].used && idx-- == 0) {
			ksnprintf(buf, (size_t)size, "%s=%s", vars[i].name, vars[i].value);
			return 0;
		}
	}
	return -1;
}
