#include "kmalloc.h"
#include "kprintf.h"
#include "cmdline.h"
#include "klog.h"
#include "kstring.h"
#include "pmm.h"
#include "rtc.h"
#include "sched.h"
#include "timer.h"
#include "version.h"
#include "vfs.h"

/*
 * /proc: read-only files that are generated on demand. Each entry has a generator that formats
 * its text into a buffer; stat() reports the generated length, read() returns the text.
 */
typedef int (*gen_fn)(char *buf, uint32_t cap);

static int gen_version(char *buf, uint32_t cap)
{
	return ksnprintf(buf, cap, "%s %s i386 (built %s %s)\n", KERNEL_NAME, KERNEL_VERSION, __DATE__,
			 __TIME__);
}

static int gen_uptime(char *buf, uint32_t cap)
{
	uint32_t ms = timer_ms();

	return ksnprintf(buf, cap, "%u.%02u\n", ms / 1000, ms % 1000 / 10);
}

static int gen_meminfo(char *buf, uint32_t cap)
{
	struct heap_stats h;

	heap_stats(&h);
	return ksnprintf(buf, cap,
			 "MemTotalKB: %u\nMemUsableKB: %u\nFramesTotal: %u\nFramesFree: %u\nMemFreeKB: %u\n"
			 "HeapTotal: %u\nHeapUsed: %u\nHeapFree: %u\nHeapLargestFree: %u\n",
			 pmm_ram_kib(), pmm_usable_kib(), pmm_total_frames(), pmm_free_frames(),
			 pmm_free_frames() * 4, h.total, h.used, h.free,
			 h.largest_free);
}

static int gen_tasks(char *buf, uint32_t cap)
{
	return sched_format(buf, cap);
}

static int gen_mounts(char *buf, uint32_t cap)
{
	return vfs_format_mounts(buf, cap);
}

static int gen_cmdline(char *buf, uint32_t cap)
{
	return ksnprintf(buf, cap, "%s\n", cmdline_all());
}

static int gen_dmesg(char *buf, uint32_t cap)
{
	return klog_copy(buf, cap);
}

static int gen_time(char *buf, uint32_t cap)
{
	struct rtc_time t;

	rtc_read(&t);
	return ksnprintf(buf, cap, "%04u-%02u-%02u %02u:%02u:%02u\n%u\n", t.year, t.month, t.day, t.hour,
			 t.minute, t.second, rtc_unix(&t));
}

struct proc_file {
	const char *name;
	gen_fn gen;
};

static const struct proc_file files[] = {
	{ "version", gen_version },
	{ "uptime", gen_uptime },
	{ "meminfo", gen_meminfo },
	{ "tasks", gen_tasks },
	{ "mounts", gen_mounts },
	{ "dmesg", gen_dmesg },
	{ "cmdline", gen_cmdline },
	{ "time", gen_time },
};
#define NFILES (sizeof files / sizeof files[0])

#define GEN_MAX 4096

static const struct proc_file *find(const char *path)
{
	unsigned i;

	while (*path == '/')
		path++;
	for (i = 0; i < NFILES; i++)
		if (!kstrcmp(files[i].name, path))
			return &files[i];
	return 0;
}

static int proc_stat(void *ctx, const char *path, struct vfs_stat *st)
{
	const struct proc_file *f;
	char *tmp;

	(void)ctx;
	st->dev = 0;
	while (*path == '/')
		path++;
	if (!*path) {
		st->size = 0;
		st->is_dir = 1;
		return FS_OK;
	}
	f = find(path);
	if (!f)
		return FS_ENOENT;
	tmp = kmalloc(GEN_MAX);
	if (!tmp)
		return FS_ENOSPC;
	st->size = (uint32_t)f->gen(tmp, GEN_MAX);
	st->is_dir = 0;
	kfree(tmp);
	return FS_OK;
}

static int proc_read(void *ctx, const char *path, void *buf, uint32_t cap)
{
	const struct proc_file *f = find(path);
	char *tmp;
	int n;

	(void)ctx;
	if (!f)
		return FS_ENOENT;
	tmp = kmalloc(GEN_MAX);
	if (!tmp)
		return FS_ENOSPC;
	n = f->gen(tmp, GEN_MAX);
	if ((uint32_t)n > cap) {
		kfree(tmp);
		return FS_ETOOBIG;
	}
	memcpy(buf, tmp, (size_t)n);
	kfree(tmp);
	return n;
}

static int proc_list(void *ctx, const char *path, struct vfs_dirent *out, int max)
{
	unsigned i;

	(void)ctx;
	while (*path == '/')
		path++;
	if (*path)
		return FS_ENOENT;
	for (i = 0; i < NFILES && (int)i < max; i++) {
		struct vfs_stat st;
		char full[VFS_NAME_MAX];

		kstrlcpy(out[i].name, files[i].name, sizeof out[i].name);
		ksnprintf(full, sizeof full, "/%s", files[i].name);
		out[i].size = proc_stat(0, full, &st) == FS_OK ? st.size : 0;
		out[i].is_dir = 0;
	}
	return (int)i;
}

static const struct vfs_ops procfs_ops = {
	"procfs", proc_stat, proc_read, 0, 0, 0, proc_list, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

void procfs_init(void)
{
	vfs_mount("/proc", &procfs_ops, 0);
}
