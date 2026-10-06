#include "console.h"
#include "file.h"
#include "kstring.h"
#include "timer.h"
#include "vfs.h"

/* ---- the /dev filesystem: a flat directory of registered character devices ---- */

#define MAX_DEVICES 16

static const struct vfs_device *devices[MAX_DEVICES];

int devfs_register(const struct vfs_device *dev)
{
	int i;

	for (i = 0; i < MAX_DEVICES; i++) {
		if (!devices[i]) {
			devices[i] = dev;
			return FS_OK;
		}
	}
	return FS_ENOSPC;
}

static const char *skip_slashes(const char *p)
{
	while (*p == '/')
		p++;
	return p;
}

static int dev_stat(void *ctx, const char *path, struct vfs_stat *st)
{
	int i;

	(void)ctx;
	path = skip_slashes(path);
	st->size = 0;
	st->dev = 0;
	if (!*path) {
		st->is_dir = 1;
		return FS_OK;
	}
	st->is_dir = 0;
	for (i = 0; i < MAX_DEVICES; i++) {
		if (devices[i] && !kstrcmp(devices[i]->name, path)) {
			st->dev = devices[i];
			return FS_OK;
		}
	}
	return FS_ENOENT;
}

static int dev_list(void *ctx, const char *path, struct vfs_dirent *out, int max)
{
	int i, n = 0;

	(void)ctx;
	if (*skip_slashes(path))
		return FS_ENOENT;
	for (i = 0; i < MAX_DEVICES && n < max; i++) {
		if (!devices[i])
			continue;
		kstrlcpy(out[n].name, devices[i]->name, sizeof out[n].name);
		out[n].size = 0;
		out[n].is_dir = 0;
		n++;
	}
	return n;
}

static const struct vfs_ops devfs_ops = {
	"devfs", dev_stat, 0, 0, 0, 0, dev_list,
};

/* ---- built-in devices ---- */

static int zero_read(void *buf, uint32_t n)
{
	memset(buf, 0, n);
	return (int)n;
}

static int sink_write(const void *buf, uint32_t n)
{
	(void)buf;
	return (int)n; /* accept and discard */
}

static uint32_t rng_state;

static int random_read(void *buf, uint32_t n)
{
	uint8_t *p = buf;
	uint32_t i;

	if (!rng_state)
		rng_state = timer_ticks() * 2654435761u + 0x9E3779B9u;
	for (i = 0; i < n; i++) {
		/* xorshift32: fast, never zero, plenty for tests (not cryptographic) */
		rng_state ^= rng_state << 13;
		rng_state ^= rng_state >> 17;
		rng_state ^= rng_state << 5;
		p[i] = (uint8_t)(rng_state >> 8);
	}
	return (int)n;
}

static int null_read(void *buf, uint32_t n)
{
	(void)buf;
	(void)n;
	return 0; /* always at end of input */
}

static int console_read(void *buf, uint32_t n)
{
	return console_stdin_read(buf, n);
}

static int console_write_dev(const void *buf, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		console_putchar(((const char *)buf)[i]);
	return (int)n;
}

static const struct vfs_device dev_null = { "null", null_read, sink_write };
static const struct vfs_device dev_console = { "console", console_read, console_write_dev };
static const struct vfs_device dev_zero = { "zero", zero_read, sink_write };
static const struct vfs_device dev_random = { "random", random_read, sink_write };

void devfs_init(void)
{
	memset(devices, 0, sizeof devices);
	vfs_mount("/dev", &devfs_ops, 0);
	devfs_register(&dev_null);
	devfs_register(&dev_console);
	devfs_register(&dev_zero);
	devfs_register(&dev_random);
}
