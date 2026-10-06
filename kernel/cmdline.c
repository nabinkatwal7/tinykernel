#include "cmdline.h"

#include "kstring.h"
#include "klog.h"
#include "vfs.h"

#define BOOT_PARAMS ((const volatile char *)0x900) /* written by the boot menu (8 bytes) */
#define MAX_LEN     200

static char line[MAX_LEN + 1];

void cmdline_init(void)
{
	int i;

	for (i = 0; i < 8 && BOOT_PARAMS[i]; i++)
		line[i] = BOOT_PARAMS[i];
	line[i] = '\0';
	klog(LOG_INFO, "cmdline: '%s'", line);
}

void cmdline_load_config(void)
{
	char cfg[100];
	int n = vfs_size("/boot.cfg"), len;

	if (n <= 0 || n >= (int)sizeof cfg || vfs_read("/boot.cfg", cfg, (uint32_t)n) < 0)
		return;
	cfg[n] = '\0';
	for (len = 0; cfg[len]; len++) /* newlines separate words just like spaces */
		if (cfg[len] == '\n' || cfg[len] == '\r' || cfg[len] == '\t')
			cfg[len] = ' ';
	len = (int)kstrlen(line);
	if (len && len < MAX_LEN)
		line[len++] = ' ';
	kstrlcpy(line + len, cfg, sizeof line - (size_t)len);
	klog(LOG_INFO, "cmdline: now '%s'", line);
}

/* Call fn for each word: (word start, word length). */
static int find_word(const char *word, int want_value, const char **value)
{
	size_t wl = kstrlen(word);
	const char *p = line;

	while (*p) {
		const char *start;
		size_t n = 0;

		while (*p == ' ')
			p++;
		start = p;
		while (p[n] && p[n] != ' ')
			n++;
		if (!want_value && n == wl && !kstrncmp(start, word, wl))
			return 1;
		if (want_value && n > wl && start[wl] == '=' && !kstrncmp(start, word, wl)) {
			*value = start + wl + 1;
			return 1;
		}
		p += n;
	}
	return 0;
}

int cmdline_has(const char *word)
{
	return find_word(word, 0, 0);
}

const char *cmdline_get(const char *key)
{
	const char *v = 0;

	return find_word(key, 1, &v) ? v : 0;
}

const char *cmdline_all(void)
{
	return line;
}
