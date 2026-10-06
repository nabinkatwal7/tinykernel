#include "shell.h"

#include "ata.h"
#include "console.h"
#include "debug.h"
#include "fs.h"
#include "io.h"
#include "keyboard.h"
#include "klog.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "pmm.h"
#include "sched.h"
#include "timer.h"
#include "user.h"
#include "version.h"

#define LINE_MAX 128
#define ARGV_MAX 16
#define HIST_MAX 16

typedef int (*cmd_fn)(int argc, char **argv);

struct command {
	const char *name;
	const char *usage;
	const char *help;
	cmd_fn fn;
};

static const struct command *cmd_table; /* set to commands[] in shell_exec() */

/* ---------------- line editing and history ---------------- */

static char hist[HIST_MAX][LINE_MAX];
static int hist_count;

static void hist_add(const char *line)
{
	if (!*line)
		return;
	if (hist_count && !kstrcmp(hist[hist_count - 1], line))
		return;
	if (hist_count == HIST_MAX) {
		memmove(hist[0], hist[1], sizeof hist[0] * (HIST_MAX - 1));
		hist_count--;
	}
	kstrlcpy(hist[hist_count++], line, LINE_MAX);
}

static void erase_line(int len)
{
	while (len--)
		console_putchar('\b');
}

static void set_line(char *buf, int *len, const char *text)
{
	erase_line(*len);
	kstrlcpy(buf, text, LINE_MAX);
	*len = (int)kstrlen(buf);
	console_write(buf);
}

static void prompt(void)
{
	console_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
	console_write("tiny> ");
	console_set_color(COLOR_WHITE, COLOR_BLACK);
}

static int readline(char *buf, int max)
{
	int len = 0, pos = hist_count;

	for (;;) {
		int k = keyboard_getkey();

		if (k == '\n') {
			console_putchar('\n');
			buf[len] = '\0';
			return len;
		} else if (k == '\b') {
			if (len > 0) {
				len--;
				console_putchar('\b');
			}
		} else if (k == KEY_UP) {
			if (pos > 0)
				set_line(buf, &len, hist[--pos]);
		} else if (k == KEY_DOWN) {
			if (pos < hist_count) {
				pos++;
				set_line(buf, &len, pos == hist_count ? "" : hist[pos]);
			}
		} else if (k == 0x0C) { /* ctrl+L */
			console_clear();
			prompt();
			buf[len] = '\0';
			console_write(buf);
		} else if (k >= 32 && k < 127 && len < max - 1) {
			buf[len++] = (char)k;
			console_putchar((char)k);
		}
	}
}

/* ---------------- parser ---------------- */

/* Splits in place on whitespace. Supports 'single', "double" quotes and backslash escapes. */
static int parse(char *line, char **argv, int max)
{
	char *p = line;
	int argc = 0;

	for (;;) {
		char *w, quote = 0;

		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p || argc >= max - 1)
			break;
		w = argv[argc++] = p;
		while (*p) {
			if (quote) {
				if (*p == quote) {
					quote = 0;
					p++;
				} else if (*p == '\\' && quote == '"' && p[1]) {
					p++;
					*w++ = *p++;
				} else {
					*w++ = *p++;
				}
			} else if (*p == '"' || *p == '\'') {
				quote = *p++;
			} else if (*p == ' ' || *p == '\t') {
				p++;
				break;
			} else if (*p == '\\' && p[1]) {
				p++;
				*w++ = *p++;
			} else {
				*w++ = *p++;
			}
		}
		*w = '\0';
	}
	argv[argc] = 0;
	return argc;
}

/* ---------------- commands ---------------- */

static int cmd_help(int argc, char **argv)
{
	const struct command *c;

	(void)argc;
	(void)argv;
	for (c = cmd_table; c->name; c++)
		console_printf("  %-9s %s\n", c->name, c->help);
	return 0;
}

static int cmd_clear(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	console_clear();
	return 0;
}

static int cmd_version(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	console_printf("%s %s (i386, built %s %s)\n", KERNEL_NAME, KERNEL_VERSION, __DATE__,
		       __TIME__);
	return 0;
}

static int cmd_echo(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (i > 1)
			console_putchar(' ');
		console_write(argv[i]);
	}
	console_putchar('\n');
	return 0;
}

static int cmd_ticks(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	console_printf("ticks=%u (%u Hz)\n", timer_ticks(), timer_hz());
	return 0;
}

static int cmd_uptime(int argc, char **argv)
{
	uint32_t s = timer_ms() / 1000;

	(void)argc;
	(void)argv;
	console_printf("up %u:%02u:%02u\n", s / 3600, s / 60 % 60, s % 60);
	return 0;
}

static int cmd_sleep(int argc, char **argv)
{
	uint32_t ms;

	if (argc < 2 || kstrtoul(argv[1], &ms)) {
		console_write("usage: sleep <ms>\n");
		return 1;
	}
	task_sleep(ms);
	return 0;
}

static int cmd_meminfo(int argc, char **argv)
{
	struct heap_stats s;

	(void)argc;
	(void)argv;
	pmm_print_map();
	heap_stats(&s);
	console_printf("heap: %u KiB total, %u used, %u free (largest %u), %u blocks\n",
		       s.total / 1024, s.used, s.free, s.largest_free, s.blocks);
	return 0;
}

static int cmd_memtest(int argc, char **argv)
{
	int bad;

	(void)argc;
	(void)argv;
	console_write("stress-testing kmalloc/kfree... ");
	bad = kmalloc_selftest();
	if (bad)
		console_printf("FAILED (%d problems)\n", bad);
	else
		console_write("ok\n");
	return bad != 0;
}

static int cmd_hexdump(int argc, char **argv)
{
	uint32_t addr, len = 64;

	if (argc < 2 || kstrtoul(argv[1], &addr) || (argc > 2 && kstrtoul(argv[2], &len))) {
		console_write("usage: hexdump <addr> [len]\n");
		return 1;
	}
	debug_hexdump((const void *)addr, len);
	return 0;
}

static int cmd_dmesg(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	klog_dump();
	return 0;
}

static int cmd_ps(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	sched_dump();
	return 0;
}

static void worker(void *arg)
{
	uint32_t id = (uint32_t)arg, i;

	for (i = 1; i <= 5; i++) {
		console_printf("[worker %u] step %u/5\n", id, i);
		task_sleep(400);
	}
}

static int cmd_spawn(int argc, char **argv)
{
	static uint32_t serial;
	uint32_t n = 1, i;
	char name[16];

	if (argc > 1 && (kstrtoul(argv[1], &n) || n == 0 || n > 8)) {
		console_write("usage: spawn [1-8]\n");
		return 1;
	}
	for (i = 0; i < n; i++) {
		serial++;
		ksnprintf(name, sizeof name, "worker%u", serial);
		if (!task_create(name, worker, (void *)serial, 1)) {
			console_write("out of memory\n");
			return 1;
		}
	}
	return 0;
}

static int cmd_kill(int argc, char **argv)
{
	uint32_t id;

	if (argc < 2 || kstrtoul(argv[1], &id)) {
		console_write("usage: kill <id>\n");
		return 1;
	}
	if (task_kill(id)) {
		console_write("cannot kill that task\n");
		return 1;
	}
	return 0;
}

static int need_fs(void)
{
	int rc = fs_mounted() ? FS_OK : fs_mount();

	if (rc != FS_OK)
		console_printf("fs: %s\n", fs_strerror(rc));
	return rc;
}

static int fs_fail(const char *what, int rc)
{
	console_printf("%s: %s\n", what, fs_strerror(rc));
	return 1;
}

static int cmd_ls(int argc, char **argv)
{
	struct fs_stat st[FS_MAX_FILES];
	int n, i;

	(void)argc;
	(void)argv;
	if (need_fs())
		return 1;
	n = fs_list(st, FS_MAX_FILES);
	for (i = 0; i < n; i++)
		console_printf("  %-19s %6u bytes  @%u\n", st[i].name, st[i].size, st[i].start_lba);
	console_printf("%d file(s), %u KiB free\n", n, fs_free_sectors() / 2);
	return 0;
}

static int cmd_cat(int argc, char **argv)
{
	char *buf;
	int n;

	if (argc < 2) {
		console_write("usage: cat <file>\n");
		return 1;
	}
	if (need_fs())
		return 1;
	n = fs_size(argv[1]);
	if (n < 0)
		return fs_fail(argv[1], n);
	buf = kmalloc((size_t)n + 1);
	if (!buf) {
		console_write("out of memory\n");
		return 1;
	}
	n = fs_read(argv[1], buf, (uint32_t)n);
	if (n < 0) {
		kfree(buf);
		return fs_fail(argv[1], n);
	}
	buf[n] = '\0';
	console_write(buf);
	if (n && buf[n - 1] != '\n')
		console_putchar('\n');
	kfree(buf);
	return 0;
}

static int cmd_write(int argc, char **argv)
{
	char text[LINE_MAX];
	int i, rc;
	size_t len = 0;

	if (argc < 3) {
		console_write("usage: write <file> <text...>\n");
		return 1;
	}
	if (need_fs())
		return 1;
	for (i = 2; i < argc; i++) {
		len += (size_t)ksnprintf(text + len, sizeof text - len, i > 2 ? " %s" : "%s",
					 argv[i]);
		if (len >= sizeof text - 2)
			break;
	}
	text[len++] = '\n';
	rc = fs_write(argv[1], text, (uint32_t)len);
	return rc ? fs_fail(argv[1], rc) : 0;
}

static int cmd_touch(int argc, char **argv)
{
	int rc;

	if (argc < 2) {
		console_write("usage: touch <file>\n");
		return 1;
	}
	if (need_fs())
		return 1;
	rc = fs_create(argv[1]);
	return rc ? fs_fail(argv[1], rc) : 0;
}

static int cmd_rm(int argc, char **argv)
{
	int rc;

	if (argc < 2) {
		console_write("usage: rm <file>\n");
		return 1;
	}
	if (need_fs())
		return 1;
	rc = fs_delete(argv[1]);
	return rc ? fs_fail(argv[1], rc) : 0;
}

static int cmd_format(int argc, char **argv)
{
	int rc = fs_format();

	(void)argc;
	(void)argv;
	if (rc)
		return fs_fail("format", rc);
	console_write("disk formatted\n");
	return 0;
}

#define CHECK(cond, msg) \
	do { if (!(cond)) { console_printf("  FAIL: %s\n", msg); fails++; } } while (0)

/* Non-destructive filesystem exercise: uses __fstest* files and cleans up after itself. */
static int cmd_fstest(int argc, char **argv)
{
	static uint8_t big[3000], back[3000];
	int fails = 0, rc;
	uint32_t i;

	(void)argc;
	(void)argv;
	if (need_fs())
		return 1;
	for (i = 0; i < sizeof big; i++)
		big[i] = (uint8_t)(i * 7 + 3);

	CHECK(fs_write("__fstest1", "hello", 5) == FS_OK, "create small file");
	CHECK(fs_size("__fstest1") == 5, "small file size");
	rc = fs_read("__fstest1", back, sizeof back);
	CHECK(rc == 5 && !memcmp(back, "hello", 5), "read small file");

	CHECK(fs_write("__fstest2", big, sizeof big) == FS_OK, "write multi-sector file");
	memset(back, 0, sizeof back);
	rc = fs_read("__fstest2", back, sizeof back);
	CHECK(rc == (int)sizeof big && !memcmp(big, back, sizeof big), "read multi-sector file");

	CHECK(fs_write("__fstest1", big, 1200) == FS_OK, "grow file (relocates extent)");
	rc = fs_read("__fstest1", back, sizeof back);
	CHECK(rc == 1200 && !memcmp(big, back, 1200), "read grown file");
	CHECK(fs_read("__fstest2", back, 10) == FS_ETOOBIG, "too-small buffer is rejected");

	CHECK(fs_create("__fstest1") == FS_EEXIST, "create existing file fails");
	CHECK(fs_read("__nope", back, sizeof back) == FS_ENOENT, "missing file reports ENOENT");
	CHECK(fs_write("bad name", "x", 1) == FS_EINVAL, "invalid name rejected");

	CHECK(fs_delete("__fstest1") == FS_OK, "delete file 1");
	CHECK(fs_delete("__fstest2") == FS_OK, "delete file 2");
	CHECK(fs_size("__fstest2") == FS_ENOENT, "deleted file is gone");

	if (fails)
		console_printf("fstest: %d check(s) failed\n", fails);
	else
		console_write("fstest: all checks passed\n");
	return fails != 0;
}

static int cmd_disk(int argc, char **argv)
{
	uint8_t sec[SECTOR_SIZE];
	uint32_t lba;

	if (!ata_present()) {
		console_write("no ATA disk\n");
		return 1;
	}
	if (argc < 2) {
		console_printf("ATA primary master: %u sectors (%u KiB)\n", ata_sectors(),
			       ata_sectors() / 2);
		return 0;
	}
	if (kstrtoul(argv[1], &lba) || ata_read(lba, 1, sec)) {
		console_write("read failed\n");
		return 1;
	}
	debug_hexdump(sec, 128);
	return 0;
}

static int cmd_install(int argc, char **argv)
{
	int rc;

	if (need_fs())
		return 1;
	if (argc < 2) {
		console_write("built-in programs (install <name> copies one to disk):\n");
		user_list_builtin();
		return 0;
	}
	rc = user_install_builtin(argv[1]);
	return rc ? fs_fail(argv[1], rc) : 0;
}

static int cmd_run(int argc, char **argv)
{
	int rc;

	if (argc < 2) {
		console_write("usage: run <program>   (see 'install' for built-ins)\n");
		return 1;
	}
	rc = user_run(argv[1]);
	console_printf("[exit code %d]\n", rc);
	return rc != 0;
}

static int cmd_history(int argc, char **argv)
{
	int i;

	(void)argc;
	(void)argv;
	for (i = 0; i < hist_count; i++)
		console_printf("%3d  %s\n", i + 1, hist[i]);
	return 0;
}

static int cmd_color(int argc, char **argv)
{
	uint32_t fg, bg = COLOR_BLACK;

	if (argc < 2 || kstrtoul(argv[1], &fg) || fg > 15 || (argc > 2 && (kstrtoul(argv[2], &bg)
								       || bg > 15))) {
		console_write("usage: color <fg 0-15> [bg 0-15]\n");
		return 1;
	}
	console_set_color((uint8_t)fg, (uint8_t)bg);
	return 0;
}

static int cmd_contest(int argc, char **argv)
{
	int i;

	(void)argc;
	(void)argv;
	console_write("console test: chars, strings, printf\n");
	console_putchar('A');
	console_putchar('\n');
	console_printf("[%s] [%c] [%d] [%u] [%x] [%08X] [%p] [%%]\n", "str", 'c', -42, 42u, 255u,
		       0xBEEFu, (void *)0x1000);
	console_printf("[%5d] [%-5d] [%05d] [%8s] [%-8s]\n", 42, 42, 42, "right", "left");
	console_write("colors: ");
	for (i = 1; i < 16; i++) {
		console_set_color((uint8_t)i, COLOR_BLACK);
		console_putchar('#');
	}
	console_set_color(COLOR_WHITE, COLOR_BLACK);
	console_write("\ntab:\tstop\tstops\n");
	return 0;
}

static int cmd_reboot(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	console_write("rebooting...\n");
	cli();
	while (inb(0x64) & 0x02)
		;
	outb(0x64, 0xFE); /* pulse the CPU reset line via the keyboard controller */
	for (;;)
		hlt();
	return 0;
}

static int cmd_halt(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	console_write("halted. You can close the window.\n");
	cli();
	for (;;)
		hlt();
	return 0;
}

static int cmd_demo(int argc, char **argv)
{
	static const char *const script[] = {
		"version",
		"echo \"Tiny OS demo: shell, memory, tasks, disk, user mode\"",
		"meminfo",
		"memtest",
		"spawn 2",
		"sleep 2600",
		"ps",
		"format",
		"write hello.txt Written by the demo",
		"ls",
		"cat hello.txt",
		"fstest",
		"rm hello.txt",
		"install",
		"run hello",
		"run counter",
		"run fault",
		"ticks",
		0,
	};
	int i;

	(void)argc;
	(void)argv;
	for (i = 0; script[i]; i++) {
		prompt();
		console_printf("%s\n", script[i]);
		shell_exec(script[i]);
		task_sleep(300);
	}
	console_write("demo complete.\n");
	return 0;
}

static const struct command commands[] = {
	{ "help",    "help",                  "list commands", cmd_help },
	{ "clear",   "clear",                 "clear the screen", cmd_clear },
	{ "version", "version",               "show the kernel version", cmd_version },
	{ "echo",    "echo [text...]",        "print arguments (quotes and \\ work)", cmd_echo },
	{ "ticks",   "ticks",                 "timer tick counter", cmd_ticks },
	{ "uptime",  "uptime",                "time since boot", cmd_uptime },
	{ "sleep",   "sleep <ms>",            "sleep via the scheduler", cmd_sleep },
	{ "meminfo", "meminfo",               "memory map, frames and heap", cmd_meminfo },
	{ "memtest", "memtest",               "stress test the allocator", cmd_memtest },
	{ "hexdump", "hexdump <addr> [len]",  "dump memory", cmd_hexdump },
	{ "dmesg",   "dmesg",                 "show the kernel log", cmd_dmesg },
	{ "ps",      "ps",                    "list tasks", cmd_ps },
	{ "spawn",   "spawn [n]",             "start demo worker tasks", cmd_spawn },
	{ "kill",    "kill <id>",             "stop a task", cmd_kill },
	{ "ls",      "ls",                    "list files", cmd_ls },
	{ "cat",     "cat <file>",            "print a file", cmd_cat },
	{ "write",   "write <file> <text>",   "create/replace a file", cmd_write },
	{ "touch",   "touch <file>",          "create an empty file", cmd_touch },
	{ "rm",      "rm <file>",             "delete a file", cmd_rm },
	{ "format",  "format",                "erase the disk and make a filesystem", cmd_format },
	{ "fstest",  "fstest",                "self-test the filesystem", cmd_fstest },
	{ "disk",    "disk [lba]",            "disk info / dump a sector", cmd_disk },
	{ "install", "install [name]",        "list/copy built-in programs to disk", cmd_install },
	{ "run",     "run <program>",         "run a program in user mode", cmd_run },
	{ "history", "history",               "show command history", cmd_history },
	{ "color",   "color <fg> [bg]",       "set text colors", cmd_color },
	{ "contest", "contest",               "console printing self-test", cmd_contest },
	{ "demo",    "demo",                  "scripted tour of everything", cmd_demo },
	{ "reboot",  "reboot",                "reset the machine", cmd_reboot },
	{ "halt",    "halt",                  "stop the CPU", cmd_halt },
	{ 0, 0, 0, 0 },
};

/* ---------------- dispatch ---------------- */

int shell_exec(const char *line)
{
	char copy[LINE_MAX];
	char *argv[ARGV_MAX];
	const struct command *c;
	int argc;

	kstrlcpy(copy, line, sizeof copy);
	argc = parse(copy, argv, ARGV_MAX);
	if (argc == 0)
		return 0;

	cmd_table = commands;
	for (c = commands; c->name; c++)
		if (!kstrcmp(c->name, argv[0]))
			return c->fn(argc, argv);

	console_printf("unknown command: %s (try 'help')\n", argv[0]);
	return 127;
}

void shell_run(void)
{
	char line[LINE_MAX];

	console_printf("%s %s - type 'help' for commands\n", KERNEL_NAME, KERNEL_VERSION);
	for (;;) {
		prompt();
		readline(line, sizeof line);
		hist_add(line);
		shell_exec(line);
	}
}
