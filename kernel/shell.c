#include "shell.h"

#include "ata.h"
#include "acpi.h"
#include "apic.h"
#include "arith.h"
#include "arp.h"
#include "bcache.h"
#include "cmdline.h"
#include "clock.h"
#include "console.h"
#include "cpustat.h"
#include "cpu.h"
#include "crashdump.h"
#include "debug.h"
#include "gdbstub.h"
#include "dhcp.h"
#include "editor.h"
#include "env.h"
#include "fat12.h"
#include "file.h"
#include "fs.h"
#include "gfx.h"
#include "gui.h"
#include "hrtime.h"
#include "icmp.h"
#include "io.h"
#include "ip.h"
#include "keyboard.h"
#include "klog.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "mouse.h"
#include "net.h"
#include "oom.h"
#include "paging.h"
#include "ksym.h"
#include "pcache.h"
#include "pipe.h"
#include "percpu.h"
#include "pci.h"
#include "pmm.h"
#include "rtc.h"
#include "sched.h"
#include "textutil.h"
#include "script.h"
#include "selftest.h"
#include "shm.h"
#include "slab.h"
#include "smp.h"
#include "smpsched.h"
#include "speaker.h"
#include "sync.h"
#include "timer.h"
#include "tcp.h"
#include "udp.h"
#include "user.h"
#include "vga.h"
#include "wm.h"
#include "vfs.h"
#include "version.h"

/* Self-test helper: needs a local 'fails' counter. */
#define CHECK(cond, msg) \
	do { if (!(cond)) { console_printf("  FAIL: %s\n", msg); fails++; } } while (0)

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

/* ---------------- Ctrl+C ---------------- */

static volatile int interrupted; /* set by Ctrl+C, polled by long-running commands */

int shell_interrupted(void)
{
	return interrupted;
}

/* Runs in the keyboard IRQ. Stops the user program if one is running, else the newest job. */
static void sigint(struct regs *r)
{
	interrupted = 1;
	if (user_is_active()) {
		if (r && (r->cs & 3)) { /* we interrupted ring 3 itself: abandon it right here */
			console_write("^C\n");
			user_abort();
		}
		user_request_abort(); /* it is inside a syscall: it checks the flag on its way out */
		return;
	}
	{
		uint32_t id = sched_newest_job();

		if (id && !task_kill(id))
			console_printf("^C (killed task %u)\n", id);
		else
			console_write("^C\n");
	}
}

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
	console_printf("tiny:%s> ", vfs_getcwd());
	console_set_color(COLOR_WHITE, COLOR_BLACK);
}

static void move_cursor(int n) /* n > 0 right, n < 0 left */
{
	while (n > 0) {
		console_putchar(CON_RIGHT);
		n--;
	}
	while (n < 0) {
		console_putchar(CON_LEFT);
		n++;
	}
}

/* Redraw buf[from..len) at the cursor, blank 'extra' cells after it, and put the cursor back at 'cur'. */
static void redraw_tail(const char *buf, int from, int len, int extra, int cur)
{
	int i;

	for (i = from; i < len; i++)
		console_putchar(buf[i]);
	for (i = 0; i < extra; i++)
		console_putchar(' ');
	move_cursor(cur - (len + extra));
}

static int complete(char *buf, int *len, int max, int *cur); /* tab completion */

/*
 * Line editor: the cursor can be anywhere in the line. Left/Right/Home/End move it, characters are inserted at
 * it, Backspace and Delete remove around it, Up/Down walk the history; Ctrl+A/E jump to the ends, Ctrl+U clears
 * the line before the cursor, Ctrl+K the rest, Ctrl+L redraws the screen, Tab completes.
 */
static int readline(char *buf, int max)
{
	int len = 0, cur = 0, pos = hist_count;

	for (;;) {
		int k = keyboard_getkey();

		if (k == '\n') {
			move_cursor(len - cur);
			console_putchar('\n');
			buf[len] = '\0';
			return len;
		} else if (k == '\b') {
			if (cur > 0) {
				memmove(buf + cur - 1, buf + cur, (size_t)(len - cur));
				len--;
				cur--;
				move_cursor(-1);
				redraw_tail(buf, cur, len, 1, cur);
			}
		} else if (k == KEY_DEL) {
			if (cur < len) {
				memmove(buf + cur, buf + cur + 1, (size_t)(len - cur - 1));
				len--;
				redraw_tail(buf, cur, len, 1, cur);
			}
		} else if (k == KEY_LEFT) {
			if (cur > 0) {
				cur--;
				move_cursor(-1);
			}
		} else if (k == KEY_RIGHT) {
			if (cur < len) {
				cur++;
				move_cursor(1);
			}
		} else if (k == KEY_HOME || k == 0x01) { /* ctrl+A */
			move_cursor(-cur);
			cur = 0;
		} else if (k == KEY_END || k == 0x05) {  /* ctrl+E */
			move_cursor(len - cur);
			cur = len;
		} else if (k == 0x0B) {                  /* ctrl+K: delete to the end of the line */
			redraw_tail(buf, len, len, len - cur, cur);
			len = cur;
		} else if (k == 0x15) {                  /* ctrl+U: delete to the start of the line */
			int gone = cur;

			move_cursor(-cur);
			memmove(buf, buf + cur, (size_t)(len - cur));
			len -= gone;
			cur = 0;
			redraw_tail(buf, 0, len, gone, 0);
		} else if (k == KEY_UP) {
			if (pos > 0) {
				move_cursor(len - cur);
				set_line(buf, &len, hist[--pos]);
				cur = len;
			}
		} else if (k == KEY_DOWN) {
			if (pos < hist_count) {
				move_cursor(len - cur);
				pos++;
				set_line(buf, &len, pos == hist_count ? "" : hist[pos]);
				cur = len;
			}
		} else if (k == '\t') {
			move_cursor(len - cur); /* completion works at the end of the line */
			cur = len;
			complete(buf, &len, max, &cur);
		} else if (k == 0x0C) { /* ctrl+L */
			console_clear();
			prompt();
			buf[len] = '\0';
			console_write(buf);
			move_cursor(cur - len);
		} else if (k >= 32 && k < 127 && len < max - 1) {
			memmove(buf + cur + 1, buf + cur, (size_t)(len - cur));
			buf[cur++] = (char)k;
			len++;
			console_putchar((char)k);
			redraw_tail(buf, cur, len, 0, cur);
		}
	}
}

/* ---------------- variable expansion ---------------- */

static int last_status;

/*
 * Expands $NAME and $? in a command line. Single quotes and \$ keep the dollar literal.
 * Unset variables expand to nothing. The result is truncated to fit.
 */
static void expand(const char *in, char *out, int size)
{
	int o = 0, single = 0;

	while (*in && o < size - 1) {
		char c = *in;

		if (c == '\'' )
			single = !single;
		if (c == '\\' && in[1] == '$') { /* keep the backslash for the parser, skip expansion */
			out[o++] = c;
			in++;
			if (o < size - 1)
				out[o++] = *in++;
			continue;
		}
		if (c == '$' && !single && in[1] == '(' && in[2] == '(') { /* $((arithmetic)) */
			char expr[96], num[16];
			const char *p = in + 3, *q;
			int depth = 0, n = 0, bad = 1;
			int32_t v = 0;

			for (q = p; *q; q++) {
				if (*q == '(')
					depth++;
				else if (*q == ')' && depth > 0)
					depth--;
				else if (*q == ')' && q[1] == ')')
					break;
			}
			if (*q) {
				for (; p < q && n < (int)sizeof expr - 1; p++)
					expr[n++] = *p;
				expr[n] = '\0';
				bad = arith_eval(expr, &v);
				in = q + 2;
			} else {
				in += 3; /* unterminated: drop the opener */
			}
			if (bad)
				kstrlcpy(num, "0", sizeof num);
			else
				ksnprintf(num, sizeof num, "%d", v);
			for (p = num; *p && o < size - 1; p++)
				out[o++] = *p;
			continue;
		}
		if (c == '$' && !single && (in[1] == '?' || in[1] == '#' || in[1] == '_' || (in[1] >= '0' && in[1] <= '9')
					    || (in[1] >= 'A' && in[1] <= 'Z') || (in[1] >= 'a' && in[1] <= 'z'))) {
			char name[ENV_NAME_MAX];
			const char *val;
			int n = 0;

			in++;
			if (*in == '?') {
				in++;
				ksnprintf(name, sizeof name, "%d", last_status);
				val = name;
			} else if (*in == '#') { /* number of script arguments */
				in++;
				ksnprintf(name, sizeof name, "%d", script_nargs());
				val = name;
			} else if (*in >= '0' && *in <= '9') { /* script parameter $0..$9 */
				val = script_param(*in++ - '0');
			} else {
				while ((*in >= 'A' && *in <= 'Z') || (*in >= 'a' && *in <= 'z') || *in == '_'
				       || (*in >= '0' && *in <= '9')) {
					if (n < ENV_NAME_MAX - 1)
						name[n++] = *in;
					in++;
				}
				name[n] = '\0';
				val = env_get(name);
			}
			while (val && *val && o < size - 1)
				out[o++] = *val++;
			continue;
		}
		out[o++] = c;
		in++;
	}
	out[o] = '\0';
}

/* Points at the last ')' of the "$((...))" starting at s (or at its third character when unterminated). */
static char *arith_end(char *s)
{
	int depth = 0;
	char *q;

	for (q = s + 3; *q; q++) {
		if (*q == '(')
			depth++;
		else if (*q == ')' && depth > 0)
			depth--;
		else if (*q == ')' && q[1] == ')')
			return q + 1;
	}
	return s + 2;
}

/* ---------------- aliases ---------------- */

#define MAX_ALIASES 16

static struct { char name[16]; char value[64]; int used; } aliases[MAX_ALIASES];

static int alias_find(const char *name, size_t len)
{
	int i;

	for (i = 0; i < MAX_ALIASES; i++)
		if (aliases[i].used && kstrlen(aliases[i].name) == len && !kstrncmp(aliases[i].name, name, len))
			return i;
	return -1;
}

/* Replace an aliased first word by its text, up to a few times (an alias may refer to another, but never to itself twice). */
static const char *apply_alias(const char *line, char *buf, int size)
{
	int round, last = -1;

	for (round = 0; round < 4; round++) {
		const char *w = line;
		size_t len;
		int a;

		while (*w == ' ' || *w == '\t')
			w++;
		for (len = 0; w[len] && w[len] != ' ' && w[len] != '\t'; len++)
			;
		a = len ? alias_find(w, len) : -1;
		if (a < 0 || a == last)
			break;
		ksnprintf(buf, (size_t)size, "%s%s", aliases[a].value, w + len);
		line = buf;
		last = a;
	}
	return line;
}

/* ---------------- command search path ---------------- */

static int is_file(const char *path)
{
	struct vfs_stat st;

	return vfs_stat(path, &st) == FS_OK && !st.is_dir;
}

static int ends_with(const char *s, const char *suffix)
{
	size_t n = kstrlen(s), m = kstrlen(suffix);

	return n >= m && !kstrcmp(s + n - m, suffix);
}

/*
 * A command that is not built into the shell: a path (contains '/'), a file called name or name.sh in a
 * directory of $PATH (default "/bin:/fat"), or a program embedded in the kernel image. Fills 'out' with
 * what to run.
 */
static int find_external(const char *name, char *out, int size)
{
	const char *path = env_get("PATH");
	size_t nlen = kstrlen(name);
	unsigned i;

	if (!path)
		path = "/bin:/fat";
	if (nlen + 4 >= (size_t)size)
		return 0;
	for (i = 0; name[i]; i++) {
		if (name[i] == '/') {
			if (is_file(name)) {
				kstrlcpy(out, name, (size_t)size);
				return 1;
			}
			ksnprintf(out, (size_t)size, "%s.sh", name);
			return is_file(out);
		}
	}
	while (*path) {
		size_t n = 0;

		while (path[n] && path[n] != ':')
			n++;
		if (n && n + nlen + 5 < (size_t)size) {
			memcpy(out, path, n);
			out[n] = '/';
			kstrlcpy(out + n + 1, name, size - (int)n - 1);
			if (is_file(out))
				return 1;
			kstrlcpy(out + n + 1 + nlen, ".sh", size - (int)(n + 1 + nlen));
			if (is_file(out))
				return 1;
		}
		path += n + (path[n] == ':');
	}
	for (i = 0; i < builtin_nprogs; i++) {
		if (!kstrcmp(builtin_progs[i].name, name)) {
			kstrlcpy(out, name, (size_t)size);
			return 1;
		}
	}
	return 0;
}

static int run_external(const char *path, int argc, char **argv)
{
	if (ends_with(path, ".sh"))
		return script_run(path, argc, argv);
	return user_run_args(path, argc, argv);
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

static int cmd_date(int argc, char **argv)
{
	struct rtc_time t;

	(void)argc;
	(void)argv;
	rtc_read(&t);
	console_printf("%04u-%02u-%02u %02u:%02u:%02u UTC (unix %u)\n", t.year, t.month, t.day,
		       t.hour, t.minute, t.second, rtc_unix(&t));
	return 0;
}

/* time <command...>: run a command and report how long it took. */
static int cmd_time(int argc, char **argv)
{
	char line[LINE_MAX];
	size_t len = 0;
	uint32_t start, ticks;
	int i, rc;

	if (argc < 2) {
		console_write("usage: time <command> [args...]\n");
		return 1;
	}
	for (i = 1; i < argc && len + 2 < sizeof line; i++)
		len += (size_t)ksnprintf(line + len, sizeof line - len, i > 1 ? " %s" : "%s", argv[i]);
	start = timer_ticks();
	rc = shell_exec(line);
	ticks = timer_ticks() - start;
	console_printf("real %u.%02us (%u ticks)\n", ticks / timer_hz(),
		       ticks % timer_hz() * 100 / timer_hz(), ticks);
	return rc;
}

static void timer_say(void *arg)
{
	console_printf("[timer] %s\n", (const char *)arg);
}

/* timer <ms> <text> | timer every <ms> <text> | timer list | timer cancel <id> */
static int cmd_timer(int argc, char **argv)
{
	static char texts[KTIMER_MAX][48];
	uint32_t ms, id;
	int periodic = argc > 1 && !kstrcmp(argv[1], "every");
	int a = periodic ? 2 : 1, slot;
	uint32_t flags;

	if (argc == 2 && !kstrcmp(argv[1], "list")) {
		ktimer_list();
		return 0;
	}
	if (argc == 3 && !kstrcmp(argv[1], "cancel") && !kstrtoul(argv[2], &id))
		return ktimer_cancel((int)id) ? (console_write("no such timer\n"), 1) : 0;
	if (argc < a + 2 || kstrtoul(argv[a], &ms)) {
		console_write("usage: timer [every] <ms> <text> | timer list | timer cancel <id>\n");
		return 1;
	}
	flags = irq_save(); /* the text must be in place before the timer can fire */
	slot = ktimer_add(ms, periodic, timer_say, 0);
	if (slot >= 0) {
		kstrlcpy(texts[slot], argv[a + 1], sizeof texts[slot]);
		ktimer_set_arg(slot, texts[slot]);
	}
	irq_restore(flags);
	if (slot < 0) {
		console_write("no free timer slots\n");
		return 1;
	}
	console_printf("timer #%d set\n", slot);
	return 0;
}

static int cmd_sleep(int argc, char **argv)
{
	uint32_t ms;

	if (argc < 2 || kstrtoul(argv[1], &ms)) {
		console_write("usage: sleep <ms>\n");
		return 1;
	}
	while (ms && !interrupted) { /* Ctrl+C aware */
		uint32_t slice = ms > 50 ? 50 : ms;

		task_sleep(slice);
		ms -= slice;
	}
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

/* Holds heap blocks on purpose so the per-task accounting in 'ps' has something to show. */
static void *hogs[8];

static int cmd_hog(int argc, char **argv)
{
	uint32_t n;
	int i;

	if (argc < 2 || kstrtoul(argv[1], &n) || n == 0) {
		console_write("usage: hog <bytes>   (see 'ps', undo with 'unhog')\n");
		return 1;
	}
	for (i = 0; i < 8; i++) {
		if (!hogs[i]) {
			hogs[i] = kmalloc(n);
			console_printf(hogs[i] ? "held %u bytes at %p\n" : "out of memory\n", n, hogs[i]);
			return hogs[i] == 0;
		}
	}
	console_write("8 blocks already held\n");
	return 1;
}

static int cmd_unhog(int argc, char **argv)
{
	int i;

	(void)argc;
	(void)argv;
	for (i = 0; i < 8; i++) {
		kfree(hogs[i]);
		hogs[i] = 0;
	}
	return 0;
}

static int cmd_heapcheck(int argc, char **argv)
{
	int errors, bad = 0;

	(void)argc;
	(void)argv;
	errors = heap_check();
	console_printf("heap check: %d error(s)\n", errors);
	if (argc > 1 && !kstrcmp(argv[1], "test")) {
		bad = kmalloc_detector_selftest();
		console_printf("detector self-test: %s\n", bad ? "FAILED" : "ok");
	}
	return errors || bad;
}

static int cmd_hexdump(int argc, char **argv)
{
	uint32_t addr, len = 64;

	if (argc < 2 || kstrtoul(argv[1], &addr) || (argc > 2 && kstrtoul(argv[2], &len))) {
		console_write("usage: hexdump <addr> [len]\n");
		return 1;
	}
	if (!paging_is_mapped(addr) || !paging_is_mapped(addr + (len ? len - 1 : 0))) {
		console_printf("%08x is not mapped\n", addr);
		return 1;
	}
	debug_hexdump((const void *)addr, len);
	return 0;
}

static int cmd_vmap(int argc, char **argv)
{
	uint32_t virt, phys, flags = PTE_RW;

	if (argc < 3 || kstrtoul(argv[1], &virt) || kstrtoul(argv[2], &phys)) {
		console_write("usage: vmap <virt> <phys> [ro]\n");
		return 1;
	}
	if (argc > 3 && !kstrcmp(argv[3], "ro"))
		flags = 0;
	if (paging_map(0, virt, phys, flags)) {
		console_write("vmap failed\n");
		return 1;
	}
	console_printf("%08x -> %08x (%s)\n", virt, phys, flags ? "rw" : "ro");
	return 0;
}

static int cmd_vunmap(int argc, char **argv)
{
	uint32_t virt;

	if (argc < 2 || kstrtoul(argv[1], &virt) || paging_unmap(0, virt)) {
		console_write("usage: vunmap <virt>   (must be mapped)\n");
		return 1;
	}
	return 0;
}

static int cmd_vtrans(int argc, char **argv)
{
	uint32_t virt, phys;

	if (argc < 2 || kstrtoul(argv[1], &virt)) {
		console_write("usage: vtrans <virt>\n");
		return 1;
	}
	phys = paging_translate(0, virt);
	if (phys == PAGING_NOT_MAPPED)
		console_printf("%08x is not mapped\n", virt);
	else
		console_printf("%08x -> %08x\n", virt, phys);
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
		if (!task_create(name, worker, (void *)serial, PRIO_DEFAULT)) {
			console_write("out of memory\n");
			return 1;
		}
	}
	return 0;
}

static int (*volatile recurse_fn)(int); /* indirect call: the compiler cannot prove the recursion is endless */

static int recurse(int n)
{
	volatile char pad[256];

	pad[0] = (char)n;
	return recurse_fn(n + 1) + pad[0];
}

static void overflow_main(void *arg)
{
	(void)arg;
	recurse_fn = recurse;
	recurse(0);
}

/* Crash test: blows the kernel stack of a task. The guard page must catch it. */
static int cmd_overflow(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	console_write("starting a task that recurses forever (system will stop)...\n");
	task_create("overflow", overflow_main, 0, PRIO_DEFAULT);
	return 0;
}

static mutex_t test_mutex = MUTEX_INIT;
static volatile uint32_t test_counter, test_done;
static int test_use_lock;

/* Read-modify-write with a yield in the middle: loses updates unless protected. */
static void mutex_worker(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < 200; i++) {
		uint32_t tmp;

		if (test_use_lock)
			mutex_lock(&test_mutex);
		tmp = test_counter;
		task_yield();
		test_counter = tmp + 1;
		if (test_use_lock)
			mutex_unlock(&test_mutex);
	}
	test_done++;
}

static uint32_t run_counter_race(int use_lock)
{
	int i;

	test_use_lock = use_lock;
	test_counter = 0;
	test_done = 0;
	for (i = 0; i < 3; i++)
		task_create("mtx-worker", mutex_worker, 0, PRIO_DEFAULT);
	while (test_done < 3)
		task_sleep(20);
	return test_counter;
}

static int cmd_mutextest(int argc, char **argv)
{
	uint32_t racy, locked;

	(void)argc;
	(void)argv;
	racy = run_counter_race(0);
	locked = run_counter_race(1);
	console_printf("3 tasks x 200 increments: without mutex %u, with mutex %u (expected 600)\n",
		       racy, locked);
	console_write(locked == 600 ? "mutextest: ok\n" : "mutextest: FAILED\n");
	return locked != 600;
}

static sem_t test_sem;
static volatile int sem_inside, sem_max;

static void sem_worker(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < 4; i++) {
		sem_wait(&test_sem);
		sem_inside++;
		if (sem_inside > sem_max)
			sem_max = sem_inside;
		task_sleep(30); /* hold the permit while others pile up */
		sem_inside--;
		sem_post(&test_sem);
	}
	test_done++;
}

/* 5 tasks compete for 2 permits: never more than 2 inside at once. */
static int cmd_semtest(int argc, char **argv)
{
	int i;

	(void)argc;
	(void)argv;
	sem_init(&test_sem, 2);
	sem_inside = sem_max = 0;
	test_done = 0;
	for (i = 0; i < 5; i++)
		task_create("sem-worker", sem_worker, 0, PRIO_DEFAULT);
	while (test_done < 5)
		task_sleep(20);
	console_printf("max concurrent holders: %d (limit 2), final permits: %d\n", sem_max,
		       sem_value(&test_sem));
	console_write(sem_max == 2 && sem_value(&test_sem) == 2 ? "semtest: ok\n" : "semtest: FAILED\n");
	return !(sem_max == 2 && sem_value(&test_sem) == 2);
}

static uint32_t eflags_now(void)
{
	uint32_t f;

	__asm__ volatile ("pushfl; popl %0" : "=r"(f));
	return f;
}

static int cmd_spintest(int argc, char **argv)
{
	spinlock_t lock = SPINLOCK_INIT;
	uint32_t flags, other, fails = 0;

	(void)argc;
	(void)argv;
	CHECK(eflags_now() & 0x200, "interrupts start enabled");
	flags = spin_lock_irqsave(&lock);
	CHECK(!(eflags_now() & 0x200), "interrupts disabled while locked");
	CHECK(lock.locked == 1, "lock is held");
	CHECK(spin_trylock(&lock, &other) == -1, "second trylock fails");
	CHECK(!(eflags_now() & 0x200), "failed trylock leaves interrupts disabled");
	spin_unlock_irqrestore(&lock, flags);
	CHECK(eflags_now() & 0x200, "interrupts restored after unlock");
	CHECK(spin_trylock(&lock, &other) == 0, "trylock succeeds when free");
	spin_unlock_irqrestore(&lock, other);

	flags = irq_save(); /* nested: must restore to 'disabled', not blindly enable */
	other = spin_lock_irqsave(&lock);
	spin_unlock_irqrestore(&lock, other);
	CHECK(!(eflags_now() & 0x200), "nested lock keeps outer disabled state");
	irq_restore(flags);

	if (fails)
		console_printf("spintest: %u check(s) failed\n", fails);
	else
		console_write("spintest: all checks passed\n");
	return fails != 0;
}

/* ---- producer/consumer over a bounded ring buffer: mutex + two counting semaphores ---- */
#define PC_SLOTS 4
#define PC_ITEMS 10 /* per producer */

static int pc_buf[PC_SLOTS];
static int pc_in, pc_out;
static mutex_t pc_lock = MUTEX_INIT;
static sem_t pc_empty, pc_full;
static volatile uint32_t pc_sum_in, pc_sum_out, pc_count_out;

static void producer(void *arg)
{
	int id = (int)arg, i;

	for (i = 1; i <= PC_ITEMS; i++) {
		int item = id * 100 + i;

		sem_wait(&pc_empty);            /* wait for a free slot */
		mutex_lock(&pc_lock);
		pc_buf[pc_in] = item;
		pc_in = (pc_in + 1) % PC_SLOTS;
		pc_sum_in += (uint32_t)item;
		mutex_unlock(&pc_lock);
		sem_post(&pc_full);
		console_printf("[producer %d] put %d\n", id, item);
		task_sleep(20 * (uint32_t)id);
	}
	test_done++;
}

static void consumer(void *arg)
{
	int id = (int)arg;

	for (;;) {
		int item;

		sem_wait(&pc_full);             /* wait for data */
		mutex_lock(&pc_lock);
		item = pc_buf[pc_out];
		pc_out = (pc_out + 1) % PC_SLOTS;
		pc_sum_out += (uint32_t)item;
		pc_count_out++;
		mutex_unlock(&pc_lock);
		sem_post(&pc_empty);
		console_printf("    [consumer %d] got %d\n", id, item);
		task_sleep(50);
	}
}

static int cmd_prodcons(int argc, char **argv)
{
	task_t *c1, *c2;
	int ok;

	(void)argc;
	(void)argv;
	pc_in = pc_out = 0;
	pc_sum_in = pc_sum_out = pc_count_out = 0;
	test_done = 0;
	sem_init(&pc_empty, PC_SLOTS);
	sem_init(&pc_full, 0);
	c1 = task_create("consumer1", consumer, (void *)1, PRIO_DEFAULT);
	c2 = task_create("consumer2", consumer, (void *)2, PRIO_DEFAULT);
	task_create("producer1", producer, (void *)1, PRIO_DEFAULT);
	task_create("producer2", producer, (void *)2, PRIO_DEFAULT);
	while (test_done < 2 || pc_count_out < 2 * PC_ITEMS)
		task_sleep(50);
	task_kill(c1->id); /* consumers loop forever; stop them (they are blocked or sleeping) */
	task_kill(c2->id);
	ok = pc_sum_in == pc_sum_out && pc_count_out == 2 * PC_ITEMS;
	console_printf("produced sum %u, consumed sum %u, items %u: %s\n", pc_sum_in, pc_sum_out,
		       pc_count_out, ok ? "ok" : "FAILED");
	return !ok;
}

static void nested_child(void *arg)
{
	(void)arg;
	task_sleep(1500);
}

static void nested_parent(void *arg)
{
	(void)arg;
	task_create("child-a", nested_child, 0, PRIO_DEFAULT);
	task_create("child-b", nested_child, 0, PRIO_DEFAULT);
	task_sleep(500); /* exits first: its children get reparented */
}

static int cmd_pstree(int argc, char **argv)
{
	(void)argc;
	if (argc > 1 && !kstrcmp(argv[1], "demo")) {
		task_create("parent", nested_parent, 0, PRIO_DEFAULT);
		task_sleep(200);
		console_write("while the parent lives:\n");
		sched_tree();
		task_sleep(600);
		console_write("after the parent exited:\n");
	}
	sched_tree();
	return 0;
}

static void fork_demo(void *arg)
{
	int local = 41; /* lives on the stack, so the child gets its own copy */
	int pid;

	(void)arg;
	pid = task_fork();
	if (pid < 0) {
		console_write("fork failed\n");
		return;
	}
	if (pid == 0) {
		local += 100;
		console_printf("[child  %u] forked, local=%d (parent unaffected)\n", task_current()->id,
			       local);
		task_sleep(200);
		console_printf("[child  %u] done, local=%d\n", task_current()->id, local);
		return;
	}
	local += 1;
	console_printf("[parent %u] forked child %d, local=%d\n", task_current()->id, pid, local);
	task_sleep(500);
	console_printf("[parent %u] done, local=%d\n", task_current()->id, local);
}

static int cmd_forktest(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	task_create("forker", fork_demo, 0, PRIO_DEFAULT);
	return 0;
}

static void exiter(void *arg)
{
	task_sleep(300);
	task_exit_with((int)arg);
}

/* Parent waits for children and collects their exit codes (including a killed one). */
static int cmd_waittest(int argc, char **argv)
{
	task_t *a = task_spawn("exit-42", exiter, (void *)42, PRIO_DEFAULT, TASKF_WAITABLE);
	task_t *b = task_spawn("exit-kill", exiter, (void *)7, PRIO_DEFAULT, TASKF_WAITABLE);
	int code_a = -100, code_b = -100, rc;
	uint32_t id_a, id_b;

	(void)argc;
	(void)argv;
	if (!a || !b)
		return 1;
	id_a = a->id;
	id_b = b->id;
	task_sleep(100);
	console_write("both children running; table while they are alive:\n");
	sched_dump();
	task_kill(id_b);
	rc = task_wait(id_a, &code_a);
	console_printf("wait(%u): rc=%d exit code %d\n", id_a, rc, code_a);
	rc = task_wait(id_b, &code_b);
	console_printf("wait(%u): rc=%d exit code %d (killed)\n", id_b, rc, code_b);
	rc = task_wait(id_a, &code_a);
	console_printf("wait(%u) again: rc=%d (already collected)\n", id_a, rc);
	console_write(code_a == 42 && code_b == -1 ? "waittest: ok\n" : "waittest: FAILED\n");
	return !(code_a == 42 && code_b == -1);
}

/* export NAME=value | export (list) */
static int cmd_export(int argc, char **argv)
{
	int i;
	char entry[ENV_NAME_MAX + ENV_VAL_MAX + 2];

	if (argc < 2) {
		for (i = 0; env_entry(i, entry, sizeof entry) == 0; i++)
			console_printf("%s\n", entry);
		return 0;
	}
	for (i = 1; i < argc; i++) {
		char *eq = argv[i];

		while (*eq && *eq != '=')
			eq++;
		if (!*eq) {
			console_printf("export: expected NAME=value, got '%s'\n", argv[i]);
			return 1;
		}
		*eq = '\0';
		if (env_set(argv[i], eq + 1)) {
			console_printf("export: cannot set '%s' (bad name or table full)\n", argv[i]);
			return 1;
		}
	}
	return 0;
}

static int cmd_unset(int argc, char **argv)
{
	int i, rc = 0;

	if (argc < 2) {
		console_write("usage: unset <NAME>...\n");
		return 1;
	}
	for (i = 1; i < argc; i++)
		rc |= env_unset(argv[i]) ? 1 : 0;
	return rc;
}

static int cmd_nice(int argc, char **argv)
{
	uint32_t id, prio;

	if (argc < 3 || kstrtoul(argv[1], &id) || kstrtoul(argv[2], &prio)
	    || task_set_priority(id, prio)) {
		console_printf("usage: nice <id> <priority %d-%d>\n", PRIO_MIN, PRIO_MAX);
		return 1;
	}
	return 0;
}

static volatile uint32_t busy_end, busy_cpu[3];

static void busy_main(void *arg)
{
	uint32_t slot = (uint32_t)arg;

	while ((int32_t)(timer_ticks() - busy_end) < 0)
		; /* burn CPU until the deadline; the timer preempts us */
	busy_cpu[slot] = task_current()->cpu_ticks;
	test_done++;
}

/* Three CPU hogs at priority 8, 5 and 2 for 3 s: priority decides the share, aging stops starvation. */
static int cmd_priotest(int argc, char **argv)
{
	static const uint32_t prio[3] = { 8, 5, 2 };
	int i, ok;

	(void)argc;
	(void)argv;
	test_done = 0;
	busy_end = timer_ticks() + 3 * timer_hz();
	for (i = 0; i < 3; i++)
		task_create("busy", busy_main, (void *)i, prio[i]);
	while (test_done < 3)
		task_sleep(50);
	for (i = 0; i < 3; i++)
		console_printf("priority %u: %u cpu ticks\n", prio[i], busy_cpu[i]);
	ok = busy_cpu[0] > busy_cpu[1] && busy_cpu[1] > busy_cpu[2] && busy_cpu[2] > 0;
	console_write(ok ? "priotest: ok (ordered by priority, nobody starved)\n" : "priotest: FAILED\n");
	return !ok;
}

static ticketlock_t smp_lock = TICKETLOCK_INIT;
static volatile uint32_t smp_counter, smp_counter_unsafe;

static void smp_hammer(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < 20000; i++) {
		ticket_lock(&smp_lock);
		smp_counter++;
		ticket_unlock(&smp_lock);
		smp_counter_unsafe++; /* the same increment with no lock: two cores can lose updates */
	}
}

/* spinsmp: both cores hammer one counter, with and without a ticket lock. */
static int cmd_spinsmp(int argc, char **argv)
{
	int fails = 0;

	(void)argc;
	(void)argv;
	if (percpu_online_count() < 2) {
		console_write("needs a second core: run 'cpus start' first\n");
		return 1;
	}
	smp_counter = smp_counter_unsafe = 0;
	CHECK(smp_post(1, smp_hammer, 0) == 0, "job handed to cpu 1");
	smp_hammer(0);                     /* this core does the same work at the same time */
	CHECK(smp_wait(1) == 0, "cpu 1 finished");
	console_printf("with ticket lock: counter = %u (expected 40000)\n", smp_counter);
	console_printf("without a lock:   counter = %u (anything below 40000 = lost updates)\n", smp_counter_unsafe);
	CHECK(smp_counter == 40000, "ticket lock keeps the count exact");
	console_write(fails ? "spinsmp: FAILED\n" : "spinsmp: ok\n");
	return fails != 0;
}

static volatile uint32_t prime_result[4];
static const uint32_t prime_limit[4] = { 2000, 3000, 5000, 10000 };

/* runs on an application processor: counts the primes below the limit by trial division */
static void prime_thread(void *arg)
{
	uint32_t slot = (uint32_t)arg, n, d, count = 0;

	for (n = 2; n < prime_limit[slot]; n++) {
		for (d = 2; d * d <= n && n % d; d++)
			;
		if (d * d > n)
			count++;
	}
	prime_result[slot] = count;
}

/* smptest: four compute threads share one application processor, preempted by its own scheduler. */
static int cmd_smptest(int argc, char **argv)
{
	static const uint32_t expect[4] = { 303, 430, 669, 1229 };
	uint32_t busy0, idle0, sw0, busy1, idle1, sw1;
	int id[4], i, fails = 0;

	(void)argc;
	(void)argv;
	if (percpu_online_count() < 2) {
		console_write("needs a second core: run 'cpus start' first\n");
		return 1;
	}
	smpsched_stats(1, &busy0, &idle0, &sw0);
	for (i = 0; i < 4; i++) {
		prime_result[i] = 0;
		id[i] = smpt_create(1, "primes", prime_thread, (void *)i);
		CHECK(id[i] > 0, "thread created on cpu 1");
	}
	for (i = 0; i < 4; i++)
		CHECK(id[i] > 0 && smpt_join(id[i], 20000) == 0, "thread finished");
	for (i = 0; i < 4; i++) {
		console_printf("  primes below %5u: %u\n", prime_limit[i], prime_result[i]);
		CHECK(prime_result[i] == expect[i], "correct prime count");
	}
	smpsched_stats(1, &busy1, &idle1, &sw1);
	console_printf("cpu 1 ran %u busy ticks, %u context switches among the threads\n", busy1 - busy0, sw1 - sw0);
	CHECK(sw1 - sw0 > 4, "the core preempted between threads");
	smpt_reap();
	console_write(fails ? "smptest: FAILED\n" : "smptest: ok\n");
	return fails != 0;
}

static volatile int where_ran[4];

static void where_thread(void *arg)
{
	where_ran[(uint32_t)arg] = this_cpu()->id; /* which core did the scheduler really use? */
}

/* smpaffinity <mask>: place a thread with a CPU affinity mask and report where it ran. */
static int cmd_smpaffinity(int argc, char **argv)
{
	uint32_t mask;
	int id, cpu;

	if (argc != 2 || kstrtoul(argv[1], &mask)) {
		console_write("usage: smpaffinity <mask>   (bit n = cpu n; thread cores are the application processors)\n");
		return 1;
	}
	where_ran[0] = -1;
	id = smpt_create_affinity(mask, "where", where_thread, (void *)0);
	if (id < 0) {
		console_printf("mask %x allows no online application processor\n", mask);
		return 1;
	}
	cpu = smpt_cpu_of(id);
	smpt_join(id, 2000);
	console_printf("thread %d placed on cpu %d, ran on cpu %d\n", id, cpu, where_ran[0]);
	smpt_reap();
	return where_ran[0] != cpu;
}

static int job_kill(const char *spec); /* "%n": the job table is near the end of the file */
static int cmd_jobs(int argc, char **argv);
static int cmd_fg(int argc, char **argv);

static int cmd_kill(int argc, char **argv)
{
	uint32_t id;

	if (argc > 1 && argv[1][0] == '%')
		return job_kill(argv[1]);
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
	struct vfs_dirent ent[FS_MAX_FILES];
	const char *path = argc > 1 ? argv[1] : vfs_getcwd();
	int n, i;

	n = vfs_list(path, ent, FS_MAX_FILES);
	if (n < 0)
		return fs_fail(path, n);
	for (i = 0; i < n; i++)
		console_printf("  %-19s %6u bytes%s\n", ent[i].name, ent[i].size,
			       ent[i].is_dir ? "  <dir>" : "");
	console_printf("%d entr%s", n, n == 1 ? "y" : "ies");
	if (fs_mounted() && !kstrcmp(path, "/"))
		console_printf(", %u KiB free", fs_free_sectors() / 2);
	console_putchar('\n');
	return 0;
}

static int cmd_mount(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	vfs_print_mounts();
	return 0;
}

static int cmd_cat(int argc, char **argv)
{
	char *buf, rdbuf[128];
	int n;

	if (argc < 2 && file_stdin_active()) { /* cat as a filter: copy standard input to the output */
		while ((n = console_stdin_read(rdbuf, sizeof rdbuf)) > 0) {
			int i;

			for (i = 0; i < n; i++)
				console_putchar(rdbuf[i]);
		}
		return 0;
	}
	if (argc < 2) {
		console_write("usage: cat <file>\n");
		return 1;
	}
	n = vfs_size(argv[1]);
	if (n < 0)
		return fs_fail(argv[1], n);
	buf = kmalloc((size_t)n + 1);
	if (!buf) {
		console_write("out of memory\n");
		return 1;
	}
	n = pcache_read(argv[1], buf, (uint32_t)n);
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
	for (i = 2; i < argc; i++) {
		len += (size_t)ksnprintf(text + len, sizeof text - len, i > 2 ? " %s" : "%s",
					 argv[i]);
		if (len >= sizeof text - 2)
			break;
	}
	text[len++] = '\n';
	rc = vfs_write(argv[1], text, (uint32_t)len);
	return rc ? fs_fail(argv[1], rc) : 0;
}

/* append <file> <text...>: add a line to the end of a file (creating it if needed). */
static int cmd_append(int argc, char **argv)
{
	char text[LINE_MAX];
	uint32_t old;
	char *buf;
	int i, rc, size;
	size_t len = 0;

	if (argc < 3) {
		console_write("usage: append <file> <text...>\n");
		return 1;
	}
	for (i = 2; i < argc; i++) {
		len += (size_t)ksnprintf(text + len, sizeof text - len, i > 2 ? " %s" : "%s", argv[i]);
		if (len >= sizeof text - 2)
			break;
	}
	text[len++] = '\n';
	size = vfs_size(argv[1]);
	if (size == FS_ENOENT)
		size = 0;
	else if (size < 0)
		return fs_fail(argv[1], size);
	old = (uint32_t)size;
	buf = kmalloc(old + len);
	if (!buf) {
		console_write("out of memory\n");
		return 1;
	}
	if (old && (rc = vfs_read(argv[1], buf, old)) < 0) {
		kfree(buf);
		return fs_fail(argv[1], rc);
	}
	memcpy(buf + old, text, len);
	rc = vfs_write(argv[1], buf, old + (uint32_t)len);
	kfree(buf);
	return rc ? fs_fail(argv[1], rc) : 0;
}

/* Last path component. */
static const char *base_name(const char *path)
{
	const char *slash = path;

	while (*path) {
		if (*path == '/' && path[1])
			slash = path + 1;
		path++;
	}
	return slash;
}

/* If 'dst' names an existing directory, the real destination is dst/basename(src). */
static void dest_path(const char *src, const char *dst, char *out, size_t size)
{
	struct vfs_stat st;

	if (vfs_stat(dst, &st) == FS_OK && st.is_dir)
		ksnprintf(out, size, "%s/%s", dst, base_name(src));
	else
		kstrlcpy(out, dst, size);
}

static int cmd_cp(int argc, char **argv)
{
	char dst[VFS_PATH_MAX], *buf;
	struct vfs_stat st;
	int n, rc;

	if (argc != 3) {
		console_write("usage: cp <source> <destination>\n");
		return 1;
	}
	rc = vfs_stat(argv[1], &st);
	if (rc < 0)
		return fs_fail(argv[1], rc);
	if (st.is_dir) {
		console_write("cp: copying directories is not supported\n");
		return 1;
	}
	dest_path(argv[1], argv[2], dst, sizeof dst);
	if (st.size == 0)
		return (rc = vfs_write(dst, "", 0)) ? fs_fail(dst, rc) : 0;
	buf = kmalloc(st.size);
	if (!buf) {
		console_write("out of memory\n");
		return 1;
	}
	n = vfs_read(argv[1], buf, st.size);
	if (n < 0) {
		kfree(buf);
		return fs_fail(argv[1], n);
	}
	rc = vfs_write(dst, buf, (uint32_t)n);
	kfree(buf);
	return rc ? fs_fail(dst, rc) : 0;
}

static int cmd_mv(int argc, char **argv)
{
	char dst[VFS_PATH_MAX];
	int rc;

	if (argc != 3) {
		console_write("usage: mv <source> <destination>\n");
		return 1;
	}
	dest_path(argv[1], argv[2], dst, sizeof dst);
	rc = vfs_rename(argv[1], dst);
	return rc ? fs_fail(argv[1], rc) : 0;
}

static int cmd_stat(int argc, char **argv)
{
	char abs[VFS_PATH_MAX];
	struct vfs_stat st;
	struct fs_stat fst;
	int i, rc = 0;

	if (argc < 2) {
		console_write("usage: stat <path>...\n");
		return 1;
	}
	for (i = 1; i < argc; i++) {
		int r = vfs_stat(argv[i], &st);

		if (r) {
			rc = fs_fail(argv[i], r);
			continue;
		}
		vfs_normalize(vfs_getcwd(), argv[i], abs, sizeof abs);
		console_printf("  File: %s\n  Type: %s\n  Size: %u bytes\n", abs,
			       st.dev ? "character device" : st.is_dir ? "directory" : "regular file",
			       st.size);
		if (!st.dev && !st.is_dir && kstrncmp(abs, "/dev/", 5) && kstrncmp(abs, "/proc/", 6)
		    && fs_stat(abs, &fst) == FS_OK)
			console_printf("  Disk: sector %u, %u sector(s)\n", fst.start_lba, fst.sectors);
	}
	return rc;
}

static int cmd_df(int argc, char **argv)
{
	struct fs_info fi;
	uint32_t kb_total, kb_used;

	(void)argc;
	(void)argv;
	console_write("Filesystem   Size(KiB)  Used(KiB)  Free(KiB)  Use%  Mounted on\n");
	if (need_fs() == 0 && fs_info(&fi) == FS_OK) {
		kb_total = fi.total_sectors / 2;
		kb_used = fi.used_sectors / 2;
		console_printf("tinyfs      %10u %10u %10u  %3u%%  /\n", kb_total, kb_used,
			       kb_total - kb_used, kb_total ? kb_used * 100 / kb_total : 0);
		console_printf("            %u file(s), %u dir(s), %u free table slot(s), largest free run %u KiB\n",
			       fi.files, fi.dirs, fi.free_entries, fi.largest_free / 2);
	}
	console_write("devfs                 -          -          -     -  /dev\n");
	console_write("procfs                -          -          -     -  /proc\n");
	return 0;
}

static int cmd_edit(int argc, char **argv)
{
	if (argc != 2) {
		console_write("usage: edit <file>   (^S save, ^Q quit)\n");
		return 1;
	}
	return editor_run(argv[1]);
}

static int cmd_cachestat(int argc, char **argv)
{
	struct bc_stats s;

	(void)argc;
	if (argc > 1 && !kstrcmp(argv[1], "drop")) {
		bc_invalidate();
		console_write("block cache emptied\n");
	}
	bc_stats_get(&s);
	console_printf("block cache: %u/%u sectors cached (%u dirty), %u hits, %u misses (%u%% hit rate)\n"
		       "             %u writes, %u written back\n",
		       s.entries, s.capacity, s.dirty, s.hits, s.misses,
		       s.hits + s.misses ? s.hits * 100 / (s.hits + s.misses) : 0, s.writes, s.writebacks);
	return 0;
}

static int cmd_sync(int argc, char **argv)
{
	int n = bc_flush();

	(void)argc;
	(void)argv;
	if (n < 0) {
		console_write("sync: disk error\n");
		return 1;
	}
	console_printf("synced %d sector(s)\n", n);
	return 0;
}

static void fsck_report(const char *msg)
{
	console_printf("  %s\n", msg);
}

static int cmd_fsck(int argc, char **argv)
{
	struct fs_check_result r;
	int repair = argc > 1 && !kstrcmp(argv[1], "-r"), rc;

	if (need_fs())
		return 1;
	console_printf("checking the filesystem%s...\n", repair ? " (repair mode)" : "");
	rc = fs_check(repair, fsck_report, &r);
	if (rc)
		return fs_fail("fsck", rc);
	if (!r.problems)
		console_write("fsck: clean\n");
	else
		console_printf("fsck: %d problem(s) found, %d repaired%s\n", r.problems, r.repaired,
			       repair || !r.problems ? "" : " (run 'fsck -r' to repair)");
	return r.problems != r.repaired;
}

/* fscorrupt <0-3>: damage the filesystem for testing fsck. */
static int cmd_fscorrupt(int argc, char **argv)
{
	uint32_t kind;

	if (argc != 2 || kstrtoul(argv[1], &kind) || fs_debug_corrupt((int)kind)) {
		console_write("usage: fscorrupt <0 leak | 1 freed-but-used | 2 orphan | 3 overlap>\n");
		return 1;
	}
	console_write("damaged on purpose; run 'fsck'\n");
	return 0;
}

/* fat info | fat ls [path] | fat cat <path>: read the FAT12 floppy image on the IDE slave. */
static int cmd_fat(int argc, char **argv)
{
	struct fat12_entry ent[32];
	const char *sub = argc > 1 ? argv[1] : "";
	int n, i;

	if (!fat12_mounted() && fat12_mount(1) != FS_OK) {
		console_write("fat: no FAT12 disk on the IDE slave\n");
		return 1;
	}
	if (!kstrcmp(sub, "info")) {
		uint32_t total, clusters, cbytes;

		fat12_info(&total, &clusters, &cbytes);
		console_printf("FAT12 volume: %u sectors, %u clusters of %u bytes\n", total, clusters, cbytes);
		return 0;
	}
	if (!kstrcmp(sub, "ls")) {
		const char *path = argc > 2 ? argv[2] : "/";

		n = fat12_list(path, ent, 32);
		if (n < 0)
			return fs_fail(path, n);
		for (i = 0; i < n; i++)
			console_printf("  %-12s %7u bytes%s\n", ent[i].name, ent[i].size,
				       ent[i].is_dir ? "  <dir>" : "");
		console_printf("%d entr%s\n", n, n == 1 ? "y" : "ies");
		return 0;
	}
	if (!kstrcmp(sub, "cat") && argc > 2) {
		struct fat12_entry st;
		char *buf;

		n = fat12_stat(argv[2], &st);
		if (n < 0)
			return fs_fail(argv[2], n);
		if (st.is_dir)
			return fs_fail(argv[2], FS_EISDIR);
		buf = kmalloc(st.size + 1);
		if (!buf) {
			console_write("out of memory\n");
			return 1;
		}
		n = fat12_read(argv[2], buf, st.size);
		if (n < 0) {
			kfree(buf);
			return fs_fail(argv[2], n);
		}
		buf[n] = '\0';
		console_write(buf);
		kfree(buf);
		return 0;
	}
	console_write("usage: fat info | fat ls [path] | fat cat <path>\n");
	return 1;
}

static int cmd_lspci(int argc, char **argv)
{
	int i, b, verbose = argc > 1 && !kstrcmp(argv[1], "-v");

	for (i = 0; i < pci_count(); i++) {
		const struct pci_dev *d = pci_get(i);

		console_printf("%02x:%02x.%u %s: %s [%04x:%04x] (rev %02x)\n", d->bus, d->slot, d->func,
			       pci_class_name(d->class_code, d->subclass), pci_vendor_name(d->vendor), d->vendor,
			       d->device, d->revision);
		if (!verbose)
			continue;
		console_printf("        class %02x%02x prog-if %02x, header type %02x, irq line %u\n", d->class_code,
			       d->subclass, d->prog_if, d->header_type & 0x7F, d->irq_line);
		for (b = 0; b < 6; b++)
			if (d->bar[b])
				console_printf("        BAR%d: %s at %08x\n", b, d->bar[b] & 1 ? "I/O ports" : "memory",
					       d->bar[b] & 1 ? d->bar[b] & ~3u : d->bar[b] & ~15u);
	}
	console_printf("%d PCI device(s)\n", pci_count());
	return 0;
}

static int cmd_cpuinfo(int argc, char **argv)
{
	static const struct { uint32_t bit; const char *name; } edx[] = {
		{ CPU_FPU, "fpu" }, { CPU_PSE, "pse" }, { CPU_TSC, "tsc" }, { CPU_MSR, "msr" },
		{ CPU_PAE, "pae" }, { CPU_CX8, "cx8" }, { CPU_APIC, "apic" }, { CPU_SEP, "sep" },
		{ CPU_PGE, "pge" }, { CPU_CMOV, "cmov" }, { CPU_MMX, "mmx" }, { CPU_FXSR, "fxsr" },
		{ CPU_SSE, "sse" }, { CPU_SSE2, "sse2" },
	};
	static const struct { uint32_t bit; const char *name; } ecx[] = {
		{ CPU_SSE3, "sse3" }, { CPU_SSSE3, "ssse3" }, { CPU_SSE41, "sse4.1" },
		{ CPU_SSE42, "sse4.2" }, { CPU_POPCNT, "popcnt" }, { CPU_HYPERV, "hypervisor" },
	};
	const struct cpu_info *c = cpu_get();
	unsigned i;
	uint32_t mhz;

	(void)argc;
	(void)argv;
	if (!c->has_cpuid) {
		console_write("this CPU has no CPUID instruction\n");
		return 1;
	}
	console_printf("vendor:   %s\n", c->vendor);
	if (c->brand[0]) {
		const char *b = c->brand;

		while (*b == ' ')
			b++;
		console_printf("model:    %s\n", b);
	}
	console_printf("family %u, model %u, stepping %u (max cpuid leaf %u)\n", c->family, c->model,
		       c->stepping, c->max_leaf);
	mhz = cpu_mhz_estimate();
	if (mhz)
		console_printf("speed:    about %u MHz (measured with the TSC)\n", mhz);
	console_write("features:");
	for (i = 0; i < sizeof edx / sizeof edx[0]; i++)
		if (c->features_edx & edx[i].bit)
			console_printf(" %s", edx[i].name);
	for (i = 0; i < sizeof ecx / sizeof ecx[0]; i++)
		if (c->features_ecx & ecx[i].bit)
			console_printf(" %s", ecx[i].name);
	console_putchar('\n');
	return 0;
}

static int cmd_mouse(int argc, char **argv)
{
	struct mouse_state m;

	if (!mouse_present()) {
		console_write("no PS/2 mouse\n");
		return 1;
	}
	if (argc > 1 && (!kstrcmp(argv[1], "on") || !kstrcmp(argv[1], "off"))) {
		mousecursor_enable(!kstrcmp(argv[1], "on"));
		return 0;
	}
	mouse_get(&m);
	console_printf("mouse at (%d, %d), buttons %c%c%c, %u packets, %u resyncs\n", m.x, m.y,
		       m.buttons & MOUSE_LEFT ? 'L' : '-', m.buttons & MOUSE_MIDDLE ? 'M' : '-',
		       m.buttons & MOUSE_RIGHT ? 'R' : '-', m.packets, m.resyncs);
	return 0;
}

/* The demos draw on the whole screen, so they cannot run while the graphical console owns it. */
static int gfx_enter(void)
{
	if (vga_in_graphics()) {
		console_write("already in graphics mode (gfxmode off)\n");
		return 1;
	}
	return vga_set_graphics();
}

/* gfxmode on|off: move the whole console onto the graphics screen and back. */
static int cmd_gfxmode(int argc, char **argv)
{
	if (argc != 2 || (kstrcmp(argv[1], "on") && kstrcmp(argv[1], "off"))) {
		console_write("usage: gfxmode on|off\n");
		return 1;
	}
	if (!kstrcmp(argv[1], "on")) {
		if (gfxcon_enable(1))
			return 1;
		console_write("graphical console: 40x12 characters\n");
	} else {
		gfxcon_enable(0);
		console_clear();
	}
	return 0;
}

/* gfx: switch to 320x200x256, draw a test card, wait for a key, switch back. */
static int cmd_gfx(int argc, char **argv)
{
	uint8_t *fb;
	int x, y;

	(void)argc;
	(void)argv;
	if (gfx_enter()) {
		console_write("gfx: cannot enter graphics mode\n");
		return 1;
	}
	fb = vga_framebuffer();
	for (y = 0; y < VGA_GFX_H; y++) {
		for (x = 0; x < VGA_GFX_W; x++) {
			uint8_t c;

			if (y < 40)
				c = (uint8_t)(x * 16 / VGA_GFX_W);                 /* the 16 text colours */
			else if (y < 80)
				c = (uint8_t)(16 + x * 16 / VGA_GFX_W);            /* grey ramp */
			else
				c = (uint8_t)(32 + ((x * 216 / VGA_GFX_W) + (y - 80) / 20 * 36) % 216); /* colour cube */
			fb[y * VGA_GFX_W + x] = c;
		}
	}
	keyboard_getkey();
	vga_set_text();
	console_clear();
	return 0;
}

/* gfxlines: pixels and lines - a fan, a box and a few steep/shallow cases, clipped at the edges. */
static int cmd_gfxlines(int argc, char **argv)
{
	int i;

	(void)argc;
	(void)argv;
	if (gfx_enter())
		return 1;
	vga_fill(0);
	for (i = 0; i < 320; i += 10) { /* fan from the bottom centre */
		gfx_line(160, 199, i, 0, (uint8_t)(32 + i % 200));
		gfx_line(160, 0, i, 199, (uint8_t)(32 + (i * 3) % 200));
	}
	gfx_hline(0, 319, 100, 15);
	gfx_vline(160, 0, 199, 15);
	gfx_line(-50, -50, 400, 300, 12);     /* runs off both ends: must clip, not wrap */
	gfx_line(10, 190, 10, 10, 14);        /* vertical via the general routine */
	gfx_line(20, 20, 300, 20, 10);        /* horizontal */
	for (i = 0; i < 64; i++)
		gfx_putpixel(i * 5, 150 + (i % 8), 11);
	keyboard_getkey();
	vga_set_text();
	console_clear();
	return 0;
}

static int cmd_gfxshapes(int argc, char **argv)
{
	int i;

	(void)argc;
	(void)argv;
	if (gfx_enter())
		return 1;
	vga_fill(1);                                   /* blue background */
	gfx_fill_rect(10, 10, 100, 60, 4);
	gfx_rect(10, 10, 100, 60, 15);
	gfx_fill_rect(20, 20, 40, 40, 14);
	gfx_rect(20, 20, 40, 40, 0);
	gfx_fill_circle(200, 50, 35, 10);
	gfx_circle(200, 50, 35, 15);
	gfx_circle(200, 50, 20, 0);
	gfx_fill_triangle(40, 180, 100, 90, 160, 180, 13);
	gfx_fill_triangle(180, 190, 300, 190, 240, 100, 11);
	for (i = 0; i < 6; i++)                        /* concentric rings */
		gfx_circle(270, 110, 5 + i * 4, (uint8_t)(32 + i * 36));
	gfx_fill_rect(-20, 190, 60, 30, 12);           /* partly off screen: clipped */
	keyboard_getkey();
	vga_set_text();
	console_clear();
	return 0;
}

static int cmd_gfxtext(int argc, char **argv)
{
	char line[41];
	int i, c;

	(void)argc;
	(void)argv;
	if (gfx_enter())
		return 1;
	vga_fill(0);
	gfx_text(8, 4, "Bitmap font rendering", 15, -1);
	gfx_text(8, 24, "white on blue", 15, 1);
	gfx_text(8, 42, "yellow on transparent", 14, -1);
	gfx_text(8, 60, "black on light grey", 0, 7);
	for (c = 32, i = 0; c < 128; c += 32, i++) { /* the printable ASCII range, 32 per row */
		int j;

		for (j = 0; j < 32; j++)
			line[j] = (char)(c + j);
		line[32] = '\0';
		gfx_text(8, 84 + i * 20, line, (uint8_t)(10 + i), -1);
	}
	gfx_rect(4, 84 + 3 * 20, gfx_text_width("centred in a box") + 8, 22, 12);
	gfx_text(8, 84 + 3 * 20 + 3, "centred in a box", 13, -1);
	keyboard_getkey();
	vga_set_text();
	console_clear();
	return 0;
}

static void paint_text(struct window *w, int cx, int cy)
{
	gfx_text(cx + 4, cy + 4, (const char *)w->data, 0, -1);
}

/* wmdemo: three draggable windows. Click a title bar and drag; any key quits. */
static int cmd_wmdemo(int argc, char **argv)
{
	static const char *const msg[3] = { "I am window 1", "Second window", "Drag my title bar" };
	struct window *w;
	struct mouse_state m;
	int i;

	(void)argc;
	(void)argv;
	if (!mouse_present()) {
		console_write("wmdemo needs the PS/2 mouse\n");
		return 1;
	}
	if (gfx_enter())
		return 1;
	wm_init(3);
	for (i = 0; i < 3; i++) {
		w = wm_create(20 + i * 50, 20 + i * 35, 140, 80, i == 0 ? "Alpha" : i == 1 ? "Beta" : "Gamma",
			      (uint8_t)(7 + i % 2 * 7));
		w->paint = paint_text;
		w->data = (void *)msg[i];
	}
	mousecursor_enable(0); /* the text-mode pointer would only paint the hidden text screen */
	mouse_get(&m);
	wm_mouse(m.x / 2, m.y / 2, m.buttons);
	wm_render();
	while (keyboard_trygetkey() < 0) {
		mouse_get(&m);
		if (wm_mouse(m.x / 2, m.y / 2, m.buttons))
			wm_render();
		task_sleep(20);
	}
	vga_set_text();
	console_clear();
	mousecursor_enable(1);
	return 0;
}

static int gui_counter;
static struct gui_button gui_buttons[3];

static void gui_inc(void *arg) { (void)arg; gui_counter++; }
static void gui_dec(void *arg) { (void)arg; gui_counter--; }
static void gui_reset(void *arg) { (void)arg; gui_counter = 0; }

static void gui_paint(struct window *w, int cx, int cy)
{
	char text[24];
	int i;

	(void)w;
	ksnprintf(text, sizeof text, "Count: %d", gui_counter);
	gfx_text(cx + 8, cy + 6, text, 0, -1);
	for (i = 0; i < 3; i++)
		gui_button_draw(&gui_buttons[i], cx, cy);
}

/* guidemo: a window with a counter and three push buttons. Any key quits. */
static int cmd_guidemo(int argc, char **argv)
{
	struct window *win;
	struct mouse_state m;
	int i;

	(void)argc;
	(void)argv;
	if (!mouse_present()) {
		console_write("guidemo needs the PS/2 mouse\n");
		return 1;
	}
	if (gfx_enter())
		return 1;
	gui_counter = 0;
	gui_buttons[0] = (struct gui_button){ 8, 30, 40, 22, "+", gui_inc, 0, 0 };
	gui_buttons[1] = (struct gui_button){ 56, 30, 40, 22, "-", gui_dec, 0, 0 };
	gui_buttons[2] = (struct gui_button){ 104, 30, 56, 22, "Reset", gui_reset, 0, 0 };
	wm_init(3);
	win = wm_create(60, 50, 180, 100, "Counter", 7);
	win->paint = gui_paint;
	mousecursor_enable(0);
	mouse_get(&m);
	wm_mouse(m.x / 2, m.y / 2, m.buttons);
	wm_render();
	while (keyboard_trygetkey() < 0) {
		int redraw;

		mouse_get(&m);
		redraw = wm_mouse(m.x / 2, m.y / 2, m.buttons);
		redraw |= gui_buttons_mouse(gui_buttons, 3, win, m.x / 2, m.y / 2, m.buttons);
		if (redraw)
			wm_render();
		task_sleep(20);
	}
	for (i = 0; i < 3; i++)
		gui_buttons[i].down = 0;
	vga_set_text();
	console_clear();
	mousecursor_enable(1);
	console_printf("final count: %d\n", gui_counter);
	return 0;
}

/* beep [freq_hz] [ms] | beep tune | beep off | beep status */
static int cmd_beep(int argc, char **argv)
{
	static const struct { uint32_t hz, ms; } tune[] = {
		{ 262, 200 }, { 294, 200 }, { 330, 200 }, { 349, 200 }, { 392, 400 }, { 392, 400 },
		{ 440, 200 }, { 440, 200 }, { 440, 200 }, { 440, 200 }, { 392, 800 },
	};
	uint32_t hz = 880, ms = 200;
	unsigned i;

	if (argc > 1 && !kstrcmp(argv[1], "tune")) {
		for (i = 0; i < sizeof tune / sizeof tune[0] && !shell_interrupted(); i++) {
			speaker_beep(tune[i].hz, tune[i].ms);
			task_sleep(30);
		}
		return 0;
	}
	if (argc > 1 && !kstrcmp(argv[1], "off")) {
		speaker_off();
		return 0;
	}
	if (argc > 1 && !kstrcmp(argv[1], "status")) {
		console_printf("speaker is %s", speaker_is_on() ? "on" : "off");
		if (speaker_is_on())
			console_printf(" (%u Hz)", speaker_freq());
		console_putchar('\n');
		return 0;
	}
	if ((argc > 1 && kstrtoul(argv[1], &hz)) || (argc > 2 && kstrtoul(argv[2], &ms)) || hz < 20 || hz > 20000
	    || ms > 10000) {
		console_write("usage: beep [20-20000 Hz] [ms <= 10000] | beep tune | beep off | beep status\n");
		return 1;
	}
	speaker_beep(hz, ms);
	return 0;
}

static int cmd_ifconfig(int argc, char **argv)
{
	char m[18];

	(void)argc;
	(void)argv;
	if (!netif.up) {
		console_write("no network interface\n");
		return 1;
	}
	console_printf("eth0: RTL8139  io %x  irq %u  link %s\n", netif.io_base, netif.irq,
		       rtl8139_link_up() ? "up" : "down");
	{
		char a[16], b[16], c[16];

		console_printf("      MAC %s  inet %s  mask %s  gateway %s\n", mac_str(netif.mac, m), ip_str(netif.ip, a),
			       ip_str(netif.netmask, b), ip_str(netif.gateway, c));
	}
	console_printf("      tx %u frames (%u errors), rx %u frames (%u errors, %u dropped)\n", netif.tx_frames,
		       netif.tx_errors, netif.rx_frames, netif.rx_errors, netif.rx_dropped);
	return 0;
}

/* netsend <text>: broadcast a raw Ethernet frame (experimental ethertype 0x88B5). */
static int cmd_netsend(int argc, char **argv)
{
	static const uint8_t bcast[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	const char *text = argc > 1 ? argv[1] : "hello from tinyos";
	int rc;

	if (!netif.up) {
		console_write("no network interface\n");
		return 1;
	}
	rc = eth_send(bcast, 0x88B5, text, (uint16_t)kstrlen(text));
	console_printf(rc ? "send failed\n" : "sent %u payload bytes (broadcast, ethertype 0x88B5)\n",
		       (uint32_t)kstrlen(text));
	return rc != 0;
}

static volatile uint32_t loop_seen;

static void loop_handler(const uint8_t *f, uint16_t len)
{
	if (len >= ETH_HLEN + 8 && !memcmp(f + ETH_HLEN, "LOOPBACK", 8))
		loop_seen++;
}

/* netloop: the card's loopback mode must hand our own frame back through the whole rx path. */
static int cmd_netloop(int argc, char **argv)
{
	static int registered;
	uint32_t before, rx_before;
	int i, rc;

	(void)argc;
	(void)argv;
	if (!netif.up) {
		console_write("no network interface\n");
		return 1;
	}
	if (!registered) {
		net_register_ethertype(0x88B6, loop_handler);
		registered = 1;
	}
	before = loop_seen;
	rx_before = netif.rx_frames;
	net_set_loopback(1);
	rc = eth_send(netif.mac, 0x88B6, "LOOPBACK payload", 16);
	for (i = 0; i < 100 && loop_seen == before; i++)
		task_sleep(10);
	net_set_loopback(0);
	if (rc || loop_seen == before) {
		console_printf("netloop: FAILED (send rc %d, rx frames +%u)\n", rc, netif.rx_frames - rx_before);
		return 1;
	}
	console_printf("netloop: frame sent and received back through IRQ -> queue -> task (rx +%u)\n",
		       netif.rx_frames - rx_before);
	return 0;
}

static int cmd_nettrace(int argc, char **argv)
{
	if (argc != 2 || (kstrcmp(argv[1], "on") && kstrcmp(argv[1], "off"))) {
		console_write("usage: nettrace on|off\n");
		return 1;
	}
	net_set_trace(!kstrcmp(argv[1], "on"));
	return 0;
}

/* arp: list the cache; arp <ip>: resolve an address on the local network. */
static int cmd_arp(int argc, char **argv)
{
	uint8_t mac[ETH_ALEN];
	char m[18], s[16];
	uint32_t ip;
	int i, n = 0;

	if (!netif.up) {
		console_write("no network interface\n");
		return 1;
	}
	if (argc > 1) {
		if (!kstrcmp(argv[1], "flush")) {
			arp_cache_clear();
			return 0;
		}
		if (ip_parse(argv[1], &ip)) {
			console_write("usage: arp [a.b.c.d | flush]\n");
			return 1;
		}
		if (arp_resolve(ip, mac, 3000)) {
			console_printf("%s: no ARP reply\n", argv[1]);
			return 1;
		}
		console_printf("%s is at %s\n", ip_str(ip, s), mac_str(mac, m));
		return 0;
	}
	for (i = 0; i < ARP_CACHE_SIZE; i++) {
		if (!arp_cache_get(i, &ip, mac)) {
			console_printf("  %-15s %s\n", ip_str(ip, s), mac_str(mac, m));
			n++;
		}
	}
	console_printf("%d ARP entr%s\n", n, n == 1 ? "y" : "ies");
	return 0;
}

/* iptest: checksum known-answer tests, byte-order helpers, address parsing. */
static int cmd_iptest(int argc, char **argv)
{
	/* the IPv4 header example from RFC 1071-style references: checksum field = 0xb861 */
	static const uint8_t hdr[20] = { 0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
					 0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7 };
	uint8_t full[20];
	uint32_t ip;
	int fails = 0;

	(void)argc;
	(void)argv;
	CHECK(ip_checksum(hdr, 20) == 0xB861, "checksum of the reference header is 0xb861");
	memcpy(full, hdr, 20);
	full[10] = 0xB8;
	full[11] = 0x61;
	CHECK(ip_checksum(full, 20) == 0, "a header carrying its checksum verifies to zero");
	full[19] ^= 1;
	CHECK(ip_checksum(full, 20) != 0, "a damaged header no longer verifies");
	{
		static const uint8_t odd[3] = { 0x01, 0x02, 0x03 };

		CHECK(ip_checksum(odd, 3) == (uint16_t)~(0x0102 + 0x0300), "odd length: last byte is zero padded");
	}
	CHECK(htons(0x1234) == 0x3412 && ntohs(0x3412) == 0x1234, "htons/ntohs swap");
	CHECK(htonl(0x11223344u) == 0x44332211u, "htonl swaps four bytes");
	CHECK(!ip_parse("192.168.1.20", &ip) && ip == IP4(192, 168, 1, 20), "parse a dotted quad");
	CHECK(ip_parse("256.1.1.1", &ip) && ip_parse("1.2.3", &ip) && ip_parse("1.2.3.4.5", &ip)
	      && ip_parse("a.b.c.d", &ip) && ip_parse("1..2.3", &ip), "malformed addresses are rejected");
	if (fails)
		console_printf("iptest: %d check(s) failed\n", fails);
	else
		console_write("iptest: all checks passed\n");
	return fails != 0;
}

/* icmptest: ping ourselves (127.0.0.1 and our own address): exercises request -> reply handling. */
static int cmd_icmptest(int argc, char **argv)
{
	uint32_t bytes = 0;
	int rtt, fails = 0;
	struct icmp_stats a, b;

	(void)argc;
	(void)argv;
	if (!netif.up) {
		console_write("no network interface\n");
		return 1;
	}
	icmp_get_stats(&a);
	rtt = icmp_ping(IP4(127, 0, 0, 1), 1, 1000, &bytes, 0);
	CHECK(rtt >= 0, "127.0.0.1 answers an echo request");
	CHECK(bytes == 32, "the reply carries our 32 bytes of data back");
	rtt = icmp_ping(netif.ip, 2, 1000, &bytes, 0);
	CHECK(rtt >= 0, "our own address answers too");
	icmp_get_stats(&b);
	CHECK(b.echo_requests_in - a.echo_requests_in == 2, "two requests reached the ICMP handler");
	CHECK(b.echo_replies_out - a.echo_replies_out == 2, "two replies were produced");
	CHECK(b.echo_replies_in - a.echo_replies_in == 2, "two replies came back");
	if (fails)
		console_printf("icmptest: %d check(s) failed\n", fails);
	else
		console_write("icmptest: all checks passed\n");
	return fails != 0;
}

/* ping <ip> [count] */
static int cmd_ping(int argc, char **argv)
{
	uint32_t ip, count = 4, bytes, sent = 0, got = 0, i, rtt_min = 0xFFFFFFFFu, rtt_max = 0, rtt_sum = 0;
	char s[16];

	if (!netif.up) {
		console_write("no network interface\n");
		return 1;
	}
	if (argc < 2 || ip_parse(argv[1], &ip) || (argc > 2 && (kstrtoul(argv[2], &count) || !count || count > 100))) {
		console_write("usage: ping <a.b.c.d> [count]\n");
		return 1;
	}
	console_printf("PING %s: 32 bytes of data\n", ip_str(ip, s));
	for (i = 0; i < count && !shell_interrupted(); i++) {
		int rtt = icmp_ping(ip, (uint16_t)(i + 1), 1500, &bytes, 0);

		sent++;
		if (rtt >= 0) {
			got++;
			rtt_sum += (uint32_t)rtt;
			if ((uint32_t)rtt < rtt_min)
				rtt_min = (uint32_t)rtt;
			if ((uint32_t)rtt > rtt_max)
				rtt_max = (uint32_t)rtt;
			console_printf("%u bytes from %s: icmp_seq=%u time=%d ms\n", bytes, s, i + 1, rtt);
		} else if (rtt == -1) {
			console_printf("icmp_seq=%u: cannot send (no route / ARP failed)\n", i + 1);
		} else {
			console_printf("icmp_seq=%u: request timed out\n", i + 1);
		}
		if (i + 1 < count)
			task_sleep(500);
	}
	console_printf("--- %s ping statistics ---\n%u sent, %u received, %u%% loss", s, sent, got,
		       sent ? (sent - got) * 100 / sent : 0);
	if (got)
		console_printf(", rtt min/avg/max = %u/%u/%u ms", rtt_min, rtt_sum / got, rtt_max);
	console_putchar('\n');
	return got == 0;
}

/* udp send <ip> <port> <text> | udp listen <port> [seconds] | udp test */
static int cmd_udp(int argc, char **argv)
{
	uint8_t buf[UDP_MAX_PAYLOAD + 1];
	uint32_t ip, port, secs = 5, from;
	uint16_t sport;
	char s[16];
	int sock, n;

	if (!netif.up) {
		console_write("no network interface\n");
		return 1;
	}
	if (argc >= 5 && !kstrcmp(argv[1], "send") && !ip_parse(argv[2], &ip) && !kstrtoul(argv[3], &port) && port < 65536) {
		sock = udp_open(0);
		if (sock < 0)
			return 1;
		n = udp_sendto(sock, ip, (uint16_t)port, argv[4], (uint16_t)kstrlen(argv[4]));
		console_printf("sent %u byte(s) from port %u: %s\n", (uint32_t)kstrlen(argv[4]), udp_local_port(sock),
			       n ? "FAILED" : "ok");
		udp_close(sock);
		return n != 0;
	}
	if (argc >= 3 && !kstrcmp(argv[1], "listen") && !kstrtoul(argv[2], &port) && port > 0 && port < 65536) {
		if (argc > 3 && kstrtoul(argv[3], &secs))
			return 1;
		sock = udp_open((uint16_t)port);
		if (sock < 0) {
			console_write("port already in use\n");
			return 1;
		}
		console_printf("listening on UDP port %u for %u s (Ctrl+C stops)\n", port, secs);
		while (!shell_interrupted()) {
			n = udp_recvfrom(sock, buf, UDP_MAX_PAYLOAD, &from, &sport, secs * 1000);
			if (n < 0)
				break;
			buf[n] = '\0';
			console_printf("from %s:%u (%d bytes): %s\n", ip_str(from, s), sport, n, (char *)buf);
			secs = 2; /* after the first datagram wait only briefly for more */
		}
		udp_close(sock);
		return 0;
	}
	if (argc >= 2 && !kstrcmp(argv[1], "test")) { /* loopback: two sockets on 127.0.0.1 */
		int a = udp_open(4000), b = udp_open(4001), ok = 0;

		if (a >= 0 && b >= 0 && !udp_sendto(a, IP4(127, 0, 0, 1), 4001, "ping over udp", 13)
		    && (n = udp_recvfrom(b, buf, UDP_MAX_PAYLOAD, &from, &sport, 500)) == 13
		    && !memcmp(buf, "ping over udp", 13) && sport == 4000 && from == IP4(127, 0, 0, 1))
			ok = 1;
		udp_close(a);
		udp_close(b);
		console_printf("udp loopback test: %s\n", ok ? "ok" : "FAILED");
		return !ok;
	}
	console_write("usage: udp send <ip> <port> <text> | udp listen <port> [secs] | udp test\n");
	return 1;
}

/* dhcp: ask the network (QEMU's built-in server) for an address. */
static int cmd_dhcp(int argc, char **argv)
{
	char a[16], b[16], c[16], d[16];
	int rc;

	(void)argc;
	(void)argv;
	if (!netif.up) {
		console_write("no network interface\n");
		return 1;
	}
	console_write("requesting a lease... ");
	rc = dhcp_acquire(3000);
	if (rc) {
		console_printf("failed (%s)\n", rc == -2 ? "no offer" : rc == -3 ? "no ACK" : "no socket");
		return 1;
	}
	console_printf("bound\n  inet %s  mask %s  gateway %s  dns %s  lease %us\n", ip_str(netif.ip, a),
		       ip_str(netif.netmask, b), ip_str(netif.gateway, c), ip_str(netif.dns, d), dhcp_lease_seconds());
	return 0;
}

/* ipconfig <ip> <mask> [gateway]: static configuration. */
static int cmd_ipconfig(int argc, char **argv)
{
	uint32_t ip, mask, gw = 0;

	if (argc < 3 || argc > 4 || ip_parse(argv[1], &ip) || ip_parse(argv[2], &mask) || (argc == 4 && ip_parse(argv[3], &gw))) {
		console_write("usage: ipconfig <ip> <netmask> [gateway]\n");
		return 1;
	}
	netif.ip = ip;
	netif.netmask = mask;
	netif.gateway = gw;
	arp_cache_clear();
	return 0;
}

/* tcp connect <ip> <port> [text] | tcp list */
static int cmd_tcp(int argc, char **argv)
{
	uint32_t ip, port;
	uint8_t buf[512];
	char s[16];
	int c, n;

	if (!netif.up) {
		console_write("no network interface\n");
		return 1;
	}
	if (argc >= 2 && !kstrcmp(argv[1], "list")) {
		int i, shown = 0;

		for (i = 0; i < TCP_MAX_CONN; i++) {
			uint16_t rp, lp;
			enum tcp_state st;

			if (!tcp_conn_info(i, &ip, &rp, &lp, &st)) {
				console_printf("  %u -> %s:%u  %s\n", lp, ip_str(ip, s), rp, tcp_state_name(st));
				shown++;
			}
		}
		console_printf("%d connection(s)\n", shown);
		return 0;
	}
	if (argc < 4 || kstrcmp(argv[1], "connect") || ip_parse(argv[2], &ip) || kstrtoul(argv[3], &port) || port == 0
	    || port > 65535) {
		console_write("usage: tcp connect <ip> <port> [text] | tcp list\n");
		return 1;
	}
	console_printf("connecting to %s:%u... ", ip_str(ip, s), port);
	c = tcp_connect(ip, (uint16_t)port, 4000);
	if (c < 0) {
		console_printf("%s\n", c == -2 ? "connection refused" : c == -1 ? "no answer" : "cannot open a connection");
		return 1;
	}
	console_printf("established (%s)\n", tcp_state_name(tcp_state(c)));
	if (argc > 4) {
		if (tcp_send(c, argv[4], (uint16_t)kstrlen(argv[4]), 3000)) {
			console_write("send failed\n");
		} else {
			n = tcp_recv(c, buf, sizeof buf - 1, 2000);
			if (n > 0) {
				buf[n] = '\0';
				console_printf("received %d byte(s): %s\n", n, (char *)buf);
			} else {
				console_write("no reply\n");
			}
		}
	}
	n = tcp_close(c);
	console_printf("connection closed%s\n", n ? " (peer did not finish the close handshake)" : "");
	return 0;
}

static int cmd_acpi(int argc, char **argv)
{
	struct acpi_table_info t;
	uint8_t rev;
	char oem[7];
	uint32_t cnt, smi;
	int typ, en, i;

	(void)argc;
	(void)argv;
	acpi_describe(&rev, oem, &cnt, &smi, &typ, &en);
	if (!acpi_table_count()) {
		console_write("no ACPI tables\n");
		return 1;
	}
	console_printf("ACPI revision %u, OEM '%s', power-off %s\n", rev, oem, acpi_available() ? "available" : "unavailable");
	console_printf("PM1a control port %x, SMI command port %x, \\_S5 sleep type %d, ACPI mode %s\n", cnt, smi, typ,
		       en ? "on" : "off");
	for (i = 0; i < acpi_table_count(); i++) {
		acpi_table_get(i, &t);
		console_printf("  %s at %08x, %u bytes\n", t.signature, t.address, t.length);
	}
	return 0;
}

/* shutdown: flush the disk cache, then power the machine off through ACPI. */
static int cmd_shutdown(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	console_write("syncing disks... ");
	bc_flush();
	console_write("powering off\n");
	task_sleep(100); /* let the screen update and the serial line drain */
	acpi_poweroff();
	console_write("power-off did not work; the system is halted\n");
	cli();
	for (;;)
		hlt();
	return 1;
}

static int cmd_cmdline(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	console_printf("kernel command line: '%s'\n", cmdline_all());
	return 0;
}

static int cmd_selftest(int argc, char **argv)
{
	return selftest_run(argc > 1 && !kstrcmp(argv[1], "-v")) != 0;
}

static int cmd_apic(int argc, char **argv)
{
	const struct acpi_madt *m = acpi_madt();
	int i;

	(void)argc;
	(void)argv;
	if (!apic_present()) {
		console_write("no local APIC\n");
		return 1;
	}
	console_printf("local APIC at %08x, id %u, version %02x, timer %s (%u counts/ms)\n", apic_base(), apic_id(),
		       apic_version() & 0xFF, apic_timer_active() ? "driving the scheduler" : "idle", apic_timer_ticks_per_ms());
	for (i = 0; i < m->ncpus; i++)
		console_printf("  cpu %d: apic id %u %s\n", i, m->cpu_apic_id[i], m->cpu_enabled[i] ? "enabled" : "disabled");
	for (i = 0; i < ioapic_count(); i++)
		console_printf("  I/O APIC %d: id %u at %08x, gsi base %u, %u inputs\n", i, m->ioapic_id[i],
			       m->ioapic_addr[i], m->ioapic_gsi_base[i], ioapic_max_redirection(i) + 1);
	for (i = 0; i < m->noverrides; i++)
		console_printf("  override: ISA irq %u -> GSI %u\n", m->override_source[i], m->override_gsi[i]);
	return 0;
}

/* hrtime: TSC-based timing vs the 10 ms tick, plus a check of the 64-bit division helpers. */
static int cmd_hrtime(int argc, char **argv)
{
	uint64_t a, b, big = 0x123456789ABCDEF0ULL;
	uint32_t t0, t1;
	int fails = 0;

	(void)argc;
	(void)argv;
	CHECK(big / 16 == 0x123456789ABCDEFULL && big % 1000 == 0x123456789ABCDEF0ULL % 1000, "64-bit div/mod helpers");
	CHECK((long long)-7 / 2 == -3 && (long long)-7 % 2 == -1, "signed 64-bit division truncates toward zero");
	CHECK(hrtime_available(), "TSC calibrated");
	if (hrtime_available()) {
		t0 = timer_ticks();
		a = hrtime_ns();
		task_sleep(100);
		b = hrtime_ns();
		t1 = timer_ticks();
		console_printf("TSC %u kHz; slept ~100 ms: hrtime says %u us, the tick counter says %u ms\n", hrtime_khz(),
			       (uint32_t)((b - a) / 1000), (t1 - t0) * 10);
		/* wide margins: under emulation the TSC and the timer tick do not run at a fixed ratio */
		CHECK(b - a > 50000000ULL && b - a < 400000000ULL, "a 100 ms sleep measures between 50 and 400 ms");
		CHECK(hrtime_ns() >= b, "the clock never goes backwards");
	}
	console_write(fails ? "hrtime: FAILED\n" : "hrtime: ok\n");
	return fails != 0;
}

/* nsleeptest: sub-tick and tick-aligned sleeps against the high-resolution clock. */
static int cmd_nsleeptest(int argc, char **argv)
{
	static const uint32_t us[] = { 100, 500, 3000, 25000, 123456 };
	unsigned i;
	int fails = 0;

	(void)argc;
	(void)argv;
	for (i = 0; i < sizeof us / sizeof us[0]; i++) {
		uint64_t a = clock_monotonic_ns(), b;
		uint32_t got;

		task_sleep_ns((uint64_t)us[i] * 1000);
		b = clock_monotonic_ns();
		got = (uint32_t)((b - a) / 1000);
		console_printf("  asked %6u us, got %6u us\n", us[i], got);
		CHECK(got >= us[i], "never wakes early");
		CHECK(got < us[i] + 30000, "wakes within 30 ms of the deadline");
	}
	console_write(fails ? "nsleeptest: FAILED\n" : "nsleeptest: ok\n");
	return fails != 0;
}

static int cmd_load(int argc, char **argv)
{
	int i;

	(void)argc;
	(void)argv;
	console_printf("CPU load (busy %%) after %u s of sampling\n", cpustat_samples());
	console_write("CPU   1s    5s    15s   60s\n");
	for (i = 0; i < percpu_count(); i++)
		console_printf("%-5d %3u%%  %3u%%  %3u%%  %3u%%\n", i, cpustat_busy_percent(i, 1), cpustat_busy_percent(i, 5),
			       cpustat_busy_percent(i, 15), cpustat_busy_percent(i, 60));
	return 0;
}

/* top [seconds]: live view, refreshed every second; any key (or Ctrl+C) quits. */
static int cmd_top(int argc, char **argv)
{
	static struct task_snapshot now[24], before[24];
	static const char *const state_names[] = { "ready", "run", "sleep", "dead", "block", "zombie" };
	uint32_t limit = 0, frames = 0, ticks_before = timer_ticks();
	int nbefore = 0;

	if (argc > 1 && kstrtoul(argv[1], &limit))
		return 1;
	nbefore = sched_snapshot(before, 24);
	for (;;) {
		int n, i, j;
		uint32_t dt, order[24], busy;
		struct mouse_state unused;

		task_sleep(1000);
		dt = timer_ticks() - ticks_before;
		ticks_before = timer_ticks();
		n = sched_snapshot(now, 24);
		for (i = 0; i < n; i++)
			order[i] = (uint32_t)i;
		for (i = 0; i < n; i++) { /* per-task ticks since the previous refresh, then sort descending */
			uint32_t prev = 0;

			for (j = 0; j < nbefore; j++)
				if (before[j].id == now[i].id)
					prev = before[j].cpu_ticks;
			now[i].cpu_ticks -= prev;
			now[i].cpu_ticks = now[i].cpu_ticks > 100000 ? 0 : now[i].cpu_ticks; /* a recycled slot */
		}
		for (i = 0; i < n; i++)
			for (j = i + 1; j < n; j++)
				if (now[order[j]].cpu_ticks > now[order[i]].cpu_ticks) {
					uint32_t t = order[i];

					order[i] = order[j];
					order[j] = t;
				}
		console_clear();
		{
			uint32_t s = timer_ms() / 1000;

			console_printf("top - up %u:%02u:%02u  %u tasks  load:", s / 3600, s / 60 % 60, s % 60, task_count());
		}
		for (i = 0; i < percpu_count(); i++)
			console_printf(" cpu%d %u%%", i, cpustat_busy_percent(i, 1));
		console_printf("\nmem: %u KiB free of %u KiB\n\n", pmm_free_frames() * 4, pmm_ram_kib());
		console_write("  ID PPID  PRIO STATE   %CPU  HEAP  NAME\n");
		(void)unused;
		for (i = 0; i < n && i < 15; i++) {
			struct task_snapshot *t = &now[order[i]];

			busy = dt ? t->cpu_ticks * 100 / dt : 0;
			console_printf("%4u %4u %5u %-6s %3u%%  %5u  %s\n", t->id, t->ppid, t->priority, state_names[t->state], busy,
				       t->heap_bytes, t->name);
		}
		console_write("\n(press any key to quit)\n");
		for (i = 0; i < n; i++)
			before[i] = now[i];
		{
			struct task_snapshot raw[24];
			int m = sched_snapshot(raw, 24);

			for (i = 0; i < m; i++)
				before[i] = raw[i]; /* the cumulative counters, not the deltas, for the next round */
			nbefore = m;
		}
		frames++;
		if (keyboard_trygetkey() >= 0 || shell_interrupted() || (limit && frames >= limit))
			break;
	}
	return 0;
}

static int cmd_cpus(int argc, char **argv)
{
	int i;

	if (argc > 1 && !kstrcmp(argv[1], "start")) {
		console_printf("%d core(s) started\n", smp_start_all());
		return 0;
	}
	console_printf("%d core(s) listed by ACPI, %d online; this is cpu %d\n", percpu_count(), percpu_online_count(),
		       this_cpu()->id);
	console_write("CPU  APIC  STATE    TIMER-IRQS  INTERRUPTS  HEARTBEAT\n");
	for (i = 0; i < percpu_count(); i++) {
		struct percpu *c = percpu_get(i);

		console_printf("%-4d %-5u %-8s %-11u %-11u %u\n", c->id, c->apic_id,
			       c->online ? (c->is_bsp ? "boot" : "online") : "offline", c->timer_irqs, c->interrupts,
			       smp_heartbeat(c->id));
	}
	return 0;
}

static int cmd_touch(int argc, char **argv)
{
	int rc;

	if (argc < 2) {
		console_write("usage: touch <file>\n");
		return 1;
	}
	rc = vfs_create(argv[1]);
	return rc ? fs_fail(argv[1], rc) : 0;
}

/* pathtest: table-driven check of vfs_normalize(). */
static int cmd_pathtest(int argc, char **argv)
{
	static const struct { const char *base, *in, *want; } t[] = {
		{ "/", "a/b", "/a/b" },
		{ "/x", "a", "/x/a" },
		{ "/x/y", "..", "/x" },
		{ "/x/y", "../z", "/x/z" },
		{ "/x", "../../..", "/" },
		{ "/x", "/abs//path/", "/abs/path" },
		{ "/x", "./a/./b/.", "/x/a/b" },
		{ "/x", "/a/b/../../c", "/c" },
		{ "/x", "", "/x" },
		{ "/", "/", "/" },
		{ "/", "///", "/" },
		{ "/x", "a/../../..", "/" },
		{ "/x", "..a/.b/...", "/x/..a/.b/..." },
	};
	char out[VFS_PATH_MAX];
	int i, fails = 0;

	(void)argc;
	(void)argv;
	for (i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
		int rc = vfs_normalize(t[i].base, t[i].in, out, sizeof out);

		if (rc || kstrcmp(out, t[i].want)) {
			console_printf("  FAIL: (%s) + (%s) -> '%s', want '%s'\n", t[i].base, t[i].in,
				       rc ? "error" : out, t[i].want);
			fails++;
		}
	}
	{
		char tiny[4];

		if (vfs_normalize("/", "abcdef", tiny, sizeof tiny) == FS_OK) {
			console_write("  FAIL: overflow not detected\n");
			fails++;
		}
	}
	if (fails)
		console_printf("pathtest: %d check(s) failed\n", fails);
	else
		console_write("pathtest: all checks passed\n");
	return fails != 0;
}

static int cmd_cd(int argc, char **argv)
{
	const char *target = argc > 1 ? argv[1] : "/";
	int rc = vfs_chdir(target);

	return rc ? fs_fail(target, rc) : 0;
}

static int cmd_pwd(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	console_printf("%s\n", vfs_getcwd());
	return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
	int i, rc = 0;

	if (argc < 2) {
		console_write("usage: mkdir <dir>...\n");
		return 1;
	}
	for (i = 1; i < argc; i++) {
		int r = vfs_mkdir(argv[i]);

		if (r)
			rc = fs_fail(argv[i], r);
	}
	return rc;
}

static int cmd_rmdir(int argc, char **argv)
{
	int i, rc = 0;

	if (argc < 2) {
		console_write("usage: rmdir <dir>...\n");
		return 1;
	}
	for (i = 1; i < argc; i++) {
		int r = vfs_rmdir(argv[i]);

		if (r)
			rc = fs_fail(argv[i], r);
	}
	return rc;
}

static int cmd_rm(int argc, char **argv)
{
	int rc;

	if (argc < 2) {
		console_write("usage: rm <file>\n");
		return 1;
	}
	rc = vfs_unlink(argv[1]);
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

	/* directories */
	{
		struct fs_stat ent[8];
		int n;

		CHECK(fs_mkdir("__d1") == FS_OK, "mkdir");
		CHECK(fs_mkdir("__d1") == FS_EEXIST, "mkdir twice fails");
		CHECK(fs_mkdir("__d1/sub") == FS_OK, "nested mkdir");
		CHECK(fs_mkdir("__nodir/x") == FS_ENOENT, "mkdir under a missing directory fails");
		CHECK(fs_write("__d1/f.txt", "in dir", 6) == FS_OK, "write inside a directory");
		CHECK(fs_write("__d1/sub/g", "deep", 4) == FS_OK, "write two levels down");
		rc = fs_read("/__d1/f.txt", back, sizeof back);
		CHECK(rc == 6 && !memcmp(back, "in dir", 6), "read with a leading slash");
		rc = fs_read("__d1//sub/g", back, sizeof back);
		CHECK(rc == 4 && !memcmp(back, "deep", 4), "read through a nested path");
		CHECK(fs_write("__d1/f.txt/x", "z", 1) == FS_ENOTDIR, "a file is not a directory");
		CHECK(fs_write("__d1", "z", 1) == FS_EISDIR, "cannot write a directory as a file");
		CHECK(fs_size("__d1") == FS_EISDIR, "size of a directory");
		n = fs_list("__d1", ent, 8);
		CHECK(n == 2, "directory lists its two children");
		CHECK(fs_write("f.txt", "root copy", 9) == FS_OK, "same name in the root is separate");
		rc = fs_read("__d1/f.txt", back, sizeof back);
		CHECK(rc == 6, "the two f.txt files do not collide");
		CHECK(fs_delete("f.txt") == FS_OK, "delete the root copy");
		CHECK(fs_rmdir("__d1") == FS_ENOTEMPTY, "rmdir refuses a non-empty directory");
		CHECK(fs_rmdir("__d1/f.txt") == FS_ENOTDIR, "rmdir on a file fails");
		CHECK(fs_delete("__d1/sub/g") == FS_OK && fs_delete("__d1/f.txt") == FS_OK, "empty it");
		CHECK(fs_rmdir("__d1/sub") == FS_OK && fs_rmdir("__d1") == FS_OK, "rmdir bottom-up");
		CHECK(fs_size("__d1/f.txt") == FS_ENOENT, "everything is gone");
	}

	if (fails)
		console_printf("fstest: %d check(s) failed\n", fails);
	else
		console_write("fstest: all checks passed\n");
	return fails != 0;
}

/* Private address spaces: a mapping made in one directory must not leak into another. */
static int cmd_pgtest(int argc, char **argv)
{
	uint32_t kdir = paging_kernel_dir(), d, f1, f2, flags;
	volatile uint32_t *virt = (volatile uint32_t *)0x40000000;
	int fails = 0;

	(void)argc;
	(void)argv;
	d = paging_new_dir();
	f1 = pmm_alloc();
	f2 = pmm_alloc();
	CHECK(d && f1 && f2, "allocate directory and frames");
	if (!d || !f1 || !f2)
		return 1;

	CHECK(!paging_map(d, 0x40000000, f1, PTE_RW), "map in new directory");
	CHECK(paging_translate(d, 0x40000000) == f1, "visible in new directory");
	CHECK(paging_translate(kdir, 0x40000000) == PAGING_NOT_MAPPED, "not visible in kernel dir");

	CHECK(!paging_map(d, 0x00500000, f2, PTE_RW), "remap a shared low slot privately");
	CHECK(paging_translate(d, 0x00500000) == f2, "private remap visible");
	CHECK(paging_translate(kdir, 0x00500000) == 0x00500000, "kernel identity map untouched");

	flags = irq_save(); /* the scheduler would switch CR3 under us */
	paging_switch(d);
	*virt = 0xC0FFEE;
	paging_switch(kdir);
	irq_restore(flags);
	CHECK(*(volatile uint32_t *)f1 == 0xC0FFEE, "write through private mapping reached the frame");

	paging_free_dir(d);
	pmm_free(f1);
	pmm_free(f2);
	if (fails)
		console_printf("pgtest: %d check(s) failed\n", fails);
	else
		console_write("pgtest: all checks passed\n");
	return fails != 0;
}

/* shm [rm <key>]: list shared memory segments */
static int cmd_shm(int argc, char **argv)
{
	int i, key, att, n = 0;
	uint32_t size, k;

	if (argc == 3 && !kstrcmp(argv[1], "rm") && !kstrtoul(argv[2], &k))
		return shm_remove((int)k) ? (console_write("no such segment\n"), 1) : 0;
	for (i = 0; i < SHM_MAX_SEGMENTS; i++) {
		if (!shm_info(i, &key, &size, &att)) {
			console_printf("  id %d key %d size %u bytes at %x, %d attach(es)\n", i, key, size, SHM_BASE + (uint32_t)i * SHM_SLOT, att);
			n++;
		}
	}
	console_printf("%d segment(s)\n", n);
	return 0;
}

/* backtrace : show the shell's own call chain */
static int cmd_backtrace(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	debug_here();
	return 0;
}

/* assert [msg] : trip a failing assertion to show the message format */
static int cmd_assert(int argc, char **argv)
{
	(void)argv;
	ASSERT_MSG(argc > 5, "needs 5 arguments, got %d", argc - 1);
	return 0;
}

/* crashdump [show|clear] : the dump written by the last panic */
static int cmd_crashdump(int argc, char **argv)
{
	if (argc > 1 && !kstrcmp(argv[1], "clear"))
		return crash_clear();
	return crash_show();
}

/* gdbstub : break into a GDB session attached to COM2 */
static int cmd_gdbstub(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	gdbstub_arm();
	return 0;
}

#define PIPE_TEST_BYTES 20000

static void pipe_writer(void *arg)
{
	struct pipe *p = arg;
	uint8_t chunk[333];
	uint32_t sent = 0, i, n;

	while (sent < PIPE_TEST_BYTES) {
		n = PIPE_TEST_BYTES - sent < sizeof chunk ? PIPE_TEST_BYTES - sent : sizeof chunk;
		for (i = 0; i < n; i++)
			chunk[i] = (uint8_t)((sent + i) % 251);
		if (pipe_write(p, chunk, n) != (int)n)
			break;
		sent += n;
	}
	pipe_close_write(p);
}

/* pipetest : a writer task pushes 20000 bytes through a 4 KiB pipe to this task (both sides must block) */
static int cmd_pipetest(int argc, char **argv)
{
	struct pipe *p = pipe_new(), *q;
	uint8_t buf[500];
	uint32_t total = 0, bad = 0, i;
	int n, ok;

	(void)argc;
	(void)argv;
	if (!p)
		return 1;
	task_create("pipe-writer", pipe_writer, p, PRIO_DEFAULT);
	while ((n = pipe_read(p, buf, sizeof buf)) > 0) {
		for (i = 0; i < (uint32_t)n; i++)
			if (buf[i] != (uint8_t)((total + i) % 251))
				bad++;
		total += (uint32_t)n;
	}
	ok = total == PIPE_TEST_BYTES && !bad;
	console_printf("pipe: %u bytes through, %u wrong, end of file after the writer closed: %s\n", total, bad,
		       ok ? "ok" : "FAILED");
	pipe_close_read(p);

	q = pipe_new(); /* writing with no reader left fails instead of blocking forever */
	pipe_close_read(q);
	n = pipe_write(q, "x", 1);
	console_printf("write with no reader: %d (expected -1)\n", n);
	pipe_close_write(q);
	return !(ok && n == -1);
}

static int sh_expect(const char *cmd, const char *want)
{
	static char out[256];
	int ok;

	console_capture_begin(out, sizeof out);
	shell_exec(cmd);
	console_capture_end();
	ok = !kstrcmp(out, want);
	if (!ok)
		console_printf("shtest: '%s' printed '%s', expected '%s'\n", cmd, out, want);
	return ok;
}

/* shtest : pipelines and redirection (needs a formatted disk) */
static int cmd_shtest(int argc, char **argv)
{
	int ok = 1;

	(void)argc;
	(void)argv;
	ok &= sh_expect("echo one | cat", "one\n");
	ok &= sh_expect("echo two | cat | cat", "two\n");
	ok &= sh_expect("echo 'a | b' | cat", "a | b\n");
	ok &= sh_expect("shv=4", "");
	ok &= sh_expect("echo $((shv*shv+1))", "17\n");
	ok &= sh_expect("let shv=shv*2", "");
	ok &= sh_expect("echo $shv $((shv > 7 && shv < 9))", "8 1\n");
	{
		static const char script[] = "# loop with continue, then an if/else\nshi=0\nwhile test $shi -lt 4\ndo\n"
					     "  let shi=shi+1\n  if test $shi = 2\n  then\n    continue\n  fi\n  echo n$shi\n"
					     "done\nif test $shi -eq 4\nthen\n  echo four\nelse\n  echo other\nfi\n";

		vfs_write("sht.sh", script, sizeof script - 1);
		ok &= sh_expect("sh sht.sh", "n1\nn3\nn4\nfour\n");
		vfs_unlink("sht.sh");
		env_unset("shi");
	}
	ok &= sh_expect("alias shx='echo aliased'", "");
	ok &= sh_expect("shx a b", "aliased a b\n");
	ok &= sh_expect("unalias shx", "");
	ok &= sh_expect("echo x > sht.txt", "");
	ok &= sh_expect("echo y >> sht.txt", "");
	ok &= sh_expect("cat sht.txt", "x\ny\n");
	ok &= sh_expect("cat < sht.txt", "x\ny\n");
	ok &= sh_expect("cat < sht.txt | cat", "x\ny\n");
	ok &= sh_expect("cat < sht.txt > sht2.txt", "");
	ok &= sh_expect("cat sht2.txt", "x\ny\n");
	ok &= sh_expect("echo 'q > r' > sht.txt", "");
	ok &= sh_expect("cat sht.txt", "q > r\n");
	env_unset("shv");
	vfs_unlink("sht.txt");
	vfs_unlink("sht2.txt");
	console_printf("shtest: %s\n", ok ? "ok" : "FAILED");
	return !ok;
}

/* sh <script> [args...] : run a script file */
static int cmd_sh(int argc, char **argv)
{
	if (argc < 2) {
		console_write("usage: sh <script> [args...]\n");
		return 1;
	}
	return script_run(argv[1], argc - 1, argv + 1);
}

static char *assignment_eq(const char *w);

/* let expr... : "let x=x+1" assigns the value of an integer expression; "let 3*4" prints it */
static int cmd_let(int argc, char **argv)
{
	int i, rc = 0;

	if (argc < 2) {
		console_write("usage: let NAME=expression | let expression\n");
		return 1;
	}
	for (i = 1; i < argc; i++) {
		char *eq = assignment_eq(argv[i]);
		int32_t v;
		char num[16];

		if (arith_eval(eq ? eq + 1 : argv[i], &v)) {
			console_printf("let: bad expression '%s'\n", argv[i]);
			return 1;
		}
		if (!eq) {
			console_printf("%d\n", v);
		} else {
			ksnprintf(num, sizeof num, "%d", v);
			*eq = 0;
			rc = env_set(argv[i], num) ? 1 : rc;
			*eq = '=';
		}
	}
	return rc;
}

static int test_int(const char *s, int32_t *v)
{
	return arith_eval(s, v);
}

/* test EXPR : status 0 if true. -z S, -n S, -e/-f/-d PATH, A = B, A != B, A -eq|-ne|-lt|-le|-gt|-ge B, ! EXPR */
static int cmd_test(int argc, char **argv)
{
	struct vfs_stat st;
	int neg = 0, n;
	char **a = argv + 1;
	int r = 0;

	argc--;
	if (argc > 0 && !kstrcmp(a[0], "!")) {
		neg = 1;
		a++;
		argc--;
	}
	n = argc;
	if (n == 1) {
		r = a[0][0] != 0;
	} else if (n == 2 && !kstrcmp(a[0], "-z")) {
		r = a[1][0] == 0;
	} else if (n == 2 && !kstrcmp(a[0], "-n")) {
		r = a[1][0] != 0;
	} else if (n == 2 && (!kstrcmp(a[0], "-e") || !kstrcmp(a[0], "-f") || !kstrcmp(a[0], "-d"))) {
		r = vfs_stat(a[1], &st) == FS_OK;
		if (r && a[0][1] == 'f')
			r = !st.is_dir;
		else if (r && a[0][1] == 'd')
			r = st.is_dir;
	} else if (n == 3 && !kstrcmp(a[1], "=")) {
		r = !kstrcmp(a[0], a[2]);
	} else if (n == 3 && !kstrcmp(a[1], "!=")) {
		r = kstrcmp(a[0], a[2]) != 0;
	} else if (n == 3 && a[1][0] == '-') {
		int32_t x, y;

		if (test_int(a[0], &x) || test_int(a[2], &y)) {
			console_write("test: integer expression expected\n");
			return 2;
		}
		if (!kstrcmp(a[1], "-eq"))
			r = x == y;
		else if (!kstrcmp(a[1], "-ne"))
			r = x != y;
		else if (!kstrcmp(a[1], "-lt"))
			r = x < y;
		else if (!kstrcmp(a[1], "-le"))
			r = x <= y;
		else if (!kstrcmp(a[1], "-gt"))
			r = x > y;
		else if (!kstrcmp(a[1], "-ge"))
			r = x >= y;
		else {
			console_printf("test: unknown operator %s\n", a[1]);
			return 2;
		}
	} else if (n != 0) {
		console_write("test: bad expression\n");
		return 2;
	}
	return (r != 0) == !neg ? 0 : 1;
}

/* alias [name=value] : define or list aliases */
static int cmd_alias(int argc, char **argv)
{
	int i, k;

	if (argc < 2) {
		for (i = 0; i < MAX_ALIASES; i++)
			if (aliases[i].used)
				console_printf("alias %s='%s'\n", aliases[i].name, aliases[i].value);
		return 0;
	}
	for (k = 1; k < argc; k++) {
		char *eq = assignment_eq(argv[k]);

		if (!eq) {
			i = alias_find(argv[k], kstrlen(argv[k]));
			if (i < 0) {
				console_printf("alias: %s not found\n", argv[k]);
				return 1;
			}
			console_printf("alias %s='%s'\n", aliases[i].name, aliases[i].value);
			continue;
		}
		*eq = 0;
		i = alias_find(argv[k], kstrlen(argv[k]));
		for (int j = 0; i < 0 && j < MAX_ALIASES; j++)
			if (!aliases[j].used)
				i = j;
		if (i < 0 || kstrlen(argv[k]) >= sizeof aliases[0].name) {
			console_write("alias: table full or name too long\n");
			*eq = '=';
			return 1;
		}
		aliases[i].used = 1;
		kstrlcpy(aliases[i].name, argv[k], sizeof aliases[i].name);
		kstrlcpy(aliases[i].value, eq + 1, sizeof aliases[i].value);
		*eq = '=';
	}
	return 0;
}

/* unalias name... */
static int cmd_unalias(int argc, char **argv)
{
	int k, rc = 0;

	if (argc < 2) {
		console_write("usage: unalias name...\n");
		return 1;
	}
	for (k = 1; k < argc; k++) {
		int i = alias_find(argv[k], kstrlen(argv[k]));

		if (i < 0) {
			console_printf("unalias: %s not found\n", argv[k]);
			rc = 1;
		} else {
			aliases[i].used = 0;
		}
	}
	return rc;
}

/* functions : list the shell functions defined by scripts */
static int cmd_functions(int argc, char **argv)
{
	int i;
	const char *n;

	(void)argc;
	(void)argv;
	for (i = 0; (n = script_function_name(i)); i++)
		console_printf("%s()\n", n);
	if (!script_function_count())
		console_write("no functions\n");
	return 0;
}

/* panic [message] : test the panic path and its stack trace */
static int cmd_panic(int argc, char **argv)
{
	panic("%s", argc > 1 ? argv[1] : "panic command");
}

/* ksym [name|0xADDR] : look up a kernel symbol; with no argument list the first few */
static int cmd_ksym(int argc, char **argv)
{
	uint32_t addr, off, i;
	const char *name;

	if (argc < 2) {
		console_printf("%u kernel symbols\n", ksym_count());
		for (i = 0; i < 8 && (name = ksym_at(i, &addr)); i++)
			console_printf("  %08x %s\n", addr, name);
		return 0;
	}
	if (argv[1][0] == '0' && argv[1][1] == 'x') {
		addr = 0;
		for (i = 2; argv[1][i]; i++)
			addr = addr * 16 + (argv[1][i] <= '9' ? argv[1][i] - '0' : (argv[1][i] | 32) - 'a' + 10);
		name = ksym_lookup(addr, &off);
		if (!name) {
			console_printf("no symbol for %08x\n", addr);
			return 1;
		}
		console_printf("%08x = %s+0x%x\n", addr, name, off);
		return 0;
	}
	addr = ksym_find(argv[1]);
	if (!addr) {
		console_printf("no symbol '%s'\n", argv[1]);
		return 1;
	}
	console_printf("%s = %08x\n", argv[1], addr);
	return 0;
}

/* pcache [drop|test] */
static int cmd_pcache(int argc, char **argv)
{
	struct pcache_stats s;
	int fails = 0;

	if (argc > 1 && !kstrcmp(argv[1], "drop"))
		pcache_drop_all();
	if (argc > 1 && !kstrcmp(argv[1], "test")) {
		static char big[6000], back[6000];
		uint32_t i;
		struct pcache_stats a, b;

		for (i = 0; i < sizeof big; i++)
			big[i] = (char)(i * 13 + 1);
		CHECK(vfs_write("/__pc.dat", big, sizeof big) == 0, "write a 6000-byte file");
		pcache_stats(&a);
		CHECK(pcache_read("/__pc.dat", back, sizeof back) == (int)sizeof back && !memcmp(big, back, sizeof big), "first read");
		CHECK(pcache_read("__pc.dat", back, sizeof back) == (int)sizeof back && !memcmp(big, back, sizeof big), "second read, other spelling of the path");
		pcache_stats(&b);
		CHECK(b.misses - a.misses == 1 && b.hits - a.hits == 1, "one miss then one hit");
		CHECK(b.pages - a.pages == 2, "two cache pages hold the file");
		big[100] = 'Z';
		CHECK(vfs_write("/__pc.dat", big, sizeof big) == 0, "rewrite the file");
		CHECK(pcache_read("/__pc.dat", back, sizeof back) == (int)sizeof back && back[100] == 'Z', "a write invalidates the cached copy");
		CHECK(pcache_shrink(100) >= 2, "memory pressure can drop every cached page");
		vfs_unlink("/__pc.dat");
		console_write(fails ? "pcache test: FAILED\n" : "pcache test: ok\n");
		return fails != 0;
	}
	pcache_stats(&s);
	console_printf("page cache: %u/%u pages, %u hits, %u misses, %u invalidations, %u reclaimed under pressure\n", s.pages,
		       PCACHE_PAGES, s.hits, s.misses, s.invalidations, s.shrunk);
	return 0;
}

static void oom_victim(void *arg)
{
	(void)arg;
	for (;;)
		task_sleep(1000);
}

/* oomtest: eat all memory; the OOM handler must kill a memory-hungry process instead of failing. */
static int cmd_oomtest(int argc, char **argv)
{
	uint32_t victim_dir, f, hog = 0, kills0 = oom_kills(), v, i, flags;
	task_t *t;
	int fails = 0;

	(void)argc;
	(void)argv;
	victim_dir = paging_new_dir();
	t = task_create("victim", oom_victim, 0, PRIO_DEFAULT);
	CHECK(victim_dir && t, "set up a victim process");
	if (!victim_dir || !t)
		return 1;
	for (i = 0; i < 200; i++) {                       /* it owns 200 user pages */
		f = pmm_alloc();
		if (!f || paging_map(victim_dir, 0x40000000u + i * PAGE_SIZE, f, PTE_RW | PTE_US))
			break;
	}
	flags = irq_save();
	t->pgdir = victim_dir;
	t->is_uproc = 1;
	irq_restore(flags);
	v = t->id;
	pcache_drop_all();                                /* so only the kill can help */

	while (pmm_free_frames() > 0 && (f = pmm_alloc()) != 0) { /* chain the frames through their first word */
		*(volatile uint32_t *)f = hog;
		hog = f;
	}
	console_printf("memory exhausted (%u free); allocating again must trigger the OOM killer\n", pmm_free_frames());
	f = pmm_alloc();
	CHECK(f != 0, "an allocation succeeds after the OOM handler ran");
	CHECK(oom_kills() == kills0 + 1, "exactly one process was killed");
	task_sleep(50);
	CHECK(task_kill(v) != 0, "the victim is really gone");
	while (hog) {                                     /* give everything back */
		uint32_t next = *(volatile uint32_t *)hog;

		pmm_free(hog);
		hog = next;
	}
	if (f)
		pmm_free(f);
	console_printf("%u frames free again\n", pmm_free_frames());
	console_write(fails ? "oomtest: FAILED\n" : "oomtest: ok\n");
	return fails != 0;
}

static int cmd_slabinfo(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	slab_print_stats();
	return 0;
}

/* Slab churn: spans several slabs, frees out of order, checks nothing leaks. */
static int cmd_slabtest(int argc, char **argv)
{
	static struct slab_cache cache;
	static void *obj[200];
	uint32_t before = pmm_free_frames(), i;
	int fails = 0;

	(void)argc;
	(void)argv;
	slab_cache_init(&cache, "slabtest", 100);
	for (i = 0; i < 200; i++) {
		obj[i] = slab_alloc(&cache);
		CHECK(obj[i], "allocation succeeded");
		if (!obj[i])
			return 1;
		memset(obj[i], (int)(i & 0xFF), 100);
	}
	CHECK(cache.nslabs >= 200 / cache.per_slab, "spans several slabs");
	for (i = 0; i < 200; i += 2)
		slab_free(&cache, obj[i]);
	for (i = 1; i < 200; i += 2) {
		uint8_t *p = obj[i];

		CHECK(p[0] == (uint8_t)i && p[99] == (uint8_t)i, "surviving object intact");
		slab_free(&cache, obj[i]);
	}
	CHECK(cache.in_use == 0, "all objects returned");
	CHECK(cache.nslabs == 1, "empty slabs released except one spare");
	CHECK(before - pmm_free_frames() == 1, "only the spare slab frame is still held");
	if (fails)
		console_printf("slabtest: %d check(s) failed\n", fails);
	else
		console_write("slabtest: all checks passed\n");
	return fails != 0;
}

/* cowtest: fork an address space copy-on-write and watch the page get copied on the first write. */
static int cmd_cowtest(int argc, char **argv)
{
	const uint32_t va = 0x40000000;
	uint32_t kdir = paging_kernel_dir(), a, b, frame, free0, flags;
	volatile uint32_t *p = (volatile uint32_t *)va;
	uint32_t *pte_a, *pte_b;
	int fails = 0;

	(void)argc;
	(void)argv;
	free0 = pmm_free_frames();
	a = paging_new_dir();
	frame = pmm_alloc();
	CHECK(a && frame, "address space and frame allocated");
	if (!a || !frame)
		return 1;
	CHECK(!paging_map(a, va, frame, PTE_RW | PTE_US), "map a user page");
	*(volatile uint32_t *)frame = 0x11111111; /* the parent's data, written through the identity map */

	b = paging_fork_dir(a);
	CHECK(b != 0, "fork the address space");
	pte_a = paging_pte(a, va);
	pte_b = paging_pte(b, va);
	CHECK(pte_a && pte_b && (*pte_a & ~0xFFFu) == frame && (*pte_b & ~0xFFFu) == frame, "both spaces map the same frame");
	CHECK(!(*pte_a & PTE_RW) && (*pte_a & PTE_COW) && !(*pte_b & PTE_RW) && (*pte_b & PTE_COW),
	      "both mappings are read-only copy-on-write");
	CHECK(pmm_refcount(frame) == 2, "the frame has two owners");

	flags = irq_save(); /* keep the scheduler from switching CR3 while we are in the child's space */
	paging_switch(b);
	CHECK(*p == 0x11111111, "the child reads the parent's data");
	*p = 0x22222222;    /* write fault -> the kernel copies the page and retries the store */
	paging_switch(kdir);
	irq_restore(flags);

	pte_a = paging_pte(a, va);
	pte_b = paging_pte(b, va);
	CHECK((*pte_b & ~0xFFFu) != frame, "the child got its own frame");
	CHECK(*(volatile uint32_t *)(*pte_b & ~0xFFFu) == 0x22222222, "the child's write landed in its copy");
	CHECK(*(volatile uint32_t *)frame == 0x11111111, "the parent's page is untouched");
	CHECK(pmm_refcount(frame) == 1, "the original frame is back to a single owner");

	flags = irq_save();
	paging_switch(a);
	*p = 0x33333333;    /* the parent is now the sole owner: its write just re-enables write access */
	paging_switch(kdir);
	irq_restore(flags);
	CHECK(*(volatile uint32_t *)frame == 0x33333333 && (*paging_pte(a, va) & ~0xFFFu) == frame,
	      "the last owner writes in place, no copy");

	paging_free_dir(b);
	paging_free_dir(a);
	pmm_free((*pte_b) & ~0xFFFu);   /* pte_b pointed into b's (now freed) table: still readable, value kept */
	pmm_free(frame);
	CHECK(pmm_free_frames() >= free0 - 1, "no frames leaked (page tables are not reclaimed by free_dir for shared slots)");
	console_write(fails ? "cowtest: FAILED\n" : "cowtest: ok\n");
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
	bc_flush(); /* the raw read below bypasses the cache */
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
		console_write("usage: run <program> [args...]   (see 'install' for built-ins)\n");
		return 1;
	}
	rc = user_run_args(argv[1], argc - 1, argv + 1);
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
	console_write("flushing disk cache... ");
	bc_flush();
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
	bc_flush();
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
	{ "date",    "date",                  "show the real-time clock", cmd_date },
	{ "time",    "time <cmd...>",         "time a command", cmd_time },
	{ "timer",   "timer [every] <ms> <txt>", "kernel timers: set/list/cancel", cmd_timer },
	{ "sleep",   "sleep <ms>",            "sleep via the scheduler", cmd_sleep },
	{ "meminfo", "meminfo",               "memory map, frames and heap", cmd_meminfo },
	{ "memtest", "memtest",               "stress test the allocator", cmd_memtest },
	{ "hog",     "hog <bytes>",           "hold heap memory (shows in ps)", cmd_hog },
	{ "unhog",   "unhog",                 "release held heap memory", cmd_unhog },
	{ "heapcheck", "heapcheck [test]",    "verify the heap / test the detector", cmd_heapcheck },
	{ "hexdump", "hexdump <addr> [len]",  "dump memory", cmd_hexdump },
	{ "vmap",    "vmap <virt> <phys> [ro]", "map a page", cmd_vmap },
	{ "vunmap",  "vunmap <virt>",         "unmap a page", cmd_vunmap },
	{ "vtrans",  "vtrans <virt>",         "translate an address", cmd_vtrans },
	{ "dmesg",  "dmesg",                 "show the kernel log", cmd_dmesg },
	{ "ps",      "ps",                    "list tasks", cmd_ps },
	{ "spawn",   "spawn [n]",             "start demo worker tasks", cmd_spawn },
	{ "overflow", "overflow",             "crash test: kernel stack overflow", cmd_overflow },
	{ "mutextest", "mutextest",           "race with/without a mutex", cmd_mutextest },
	{ "semtest", "semtest",             "semaphore limits concurrency", cmd_semtest },
	{ "spintest", "spintest",             "spinlock IRQ-safety self-test", cmd_spintest },
	{ "prodcons", "prodcons",             "producer/consumer demo", cmd_prodcons },
	{ "pstree",  "pstree [demo]",         "task tree by parent", cmd_pstree },
	{ "forktest", "forktest",             "fork-style task cloning", cmd_forktest },
	{ "waittest", "waittest",             "exit codes via task_wait", cmd_waittest },
	{ "export",  "export [NAME=value]",   "set/list environment variables", cmd_export },
	{ "unset",   "unset <NAME>",          "remove an environment variable", cmd_unset },
	{ "nice",    "nice <id> <prio>",      "set a task priority", cmd_nice },
	{ "priotest", "priotest",             "priority + aging scheduler test", cmd_priotest },
	{ "spinsmp", "spinsmp",               "two-core spinlock test", cmd_spinsmp },
	{ "smptest", "smptest",               "threads on the second core", cmd_smptest },
	{ "smpaffinity", "smpaffinity <mask>",   "place a thread by CPU mask", cmd_smpaffinity },
	{ "kill",  "kill <id>",             "stop a task", cmd_kill },
	{ "ls",      "ls",                    "list files", cmd_ls },
	{ "mount",   "mount",                 "list mounted filesystems", cmd_mount },
	{ "cat",     "cat <file>",            "print a file", cmd_cat },
	{ "write",   "write <file> <text>",   "create/replace a file", cmd_write },
	{ "append",  "append <file> <text>",  "append a line to a file", cmd_append },
	{ "mv",      "mv <src> <dst>",        "move or rename", cmd_mv },
	{ "cp",      "cp <src> <dst>",        "copy a file", cmd_cp },
	{ "stat",    "stat <path>",           "show file information", cmd_stat },
	{ "df",      "df",                    "disk space usage", cmd_df },
	{ "edit",    "edit <file>",           "full-screen text editor", cmd_edit },
	{ "cachestat", "cachestat [drop]",    "block cache statistics", cmd_cachestat },
	{ "sync",    "sync",                  "flush cached writes to disk", cmd_sync },
	{ "fsck",    "fsck [-r]",             "check (and repair) the filesystem", cmd_fsck },
	{ "fscorrupt", "fscorrupt <0-3>",     "damage the fs for testing fsck", cmd_fscorrupt },
	{ "fat",     "fat info|ls|cat",       "read the FAT12 image on the IDE slave", cmd_fat },
	{ "lspci",   "lspci [-v]",            "list PCI devices", cmd_lspci },
	{ "cpuinfo", "cpuinfo",               "CPU identification and speed", cmd_cpuinfo },
	{ "mouse",   "mouse",                 "PS/2 mouse position and buttons", cmd_mouse },
	{ "gfx",     "gfx",                   "graphics mode test card", cmd_gfx },
	{ "gfxlines", "gfxlines",            "pixel and line drawing demo", cmd_gfxlines },
	{ "gfxshapes", "gfxshapes",          "rectangles, circles and triangles demo", cmd_gfxshapes },
	{ "gfxtext", "gfxtext",              "bitmap font demo", cmd_gfxtext },
	{ "gfxmode", "gfxmode on|off",       "run the console on the graphics screen", cmd_gfxmode },
	{ "wmdemo",  "wmdemo",                "window manager demo (mouse)", cmd_wmdemo },
	{ "guidemo", "guidemo",              "button widget demo (mouse)", cmd_guidemo },
	{ "beep",    "beep [hz] [ms]",        "PC speaker: tone, tune, off, status", cmd_beep },
	{ "ifconfig", "ifconfig",              "network interface status", cmd_ifconfig },
	{ "netsend", "netsend [text]",         "send a raw Ethernet frame", cmd_netsend },
	{ "netloop", "netloop",               "self-test the receive path in loopback", cmd_netloop },
	{ "nettrace", "nettrace on|off",      "show every received frame", cmd_nettrace },
	{ "arp",     "arp [ip|flush]",        "ARP cache / resolve an address", cmd_arp },
	{ "iptest",  "iptest",                "IPv4 checksum / parsing self-test", cmd_iptest },
	{ "icmptest", "icmptest",             "ICMP echo self-test (loopback)", cmd_icmptest },
	{ "ping",    "ping <ip> [count]",     "send ICMP echo requests", cmd_ping },
	{ "udp",     "udp send|listen|test",  "UDP datagrams", cmd_udp },
	{ "dhcp",    "dhcp",                  "get an address via DHCP", cmd_dhcp },
	{ "ipconfig", "ipconfig <ip> <mask> [gw]", "set a static address", cmd_ipconfig },
	{ "tcp",     "tcp connect|list",      "TCP client (handshake, send, receive)", cmd_tcp },
	{ "acpi",    "acpi",                  "show ACPI tables and power-off info", cmd_acpi },
	{ "shutdown", "shutdown",             "sync and power off", cmd_shutdown },
	{ "cmdline", "cmdline",               "show the kernel parameters", cmd_cmdline },
	{ "selftest", "selftest [-v]",         "run every self-test", cmd_selftest },
	{ "apic",    "apic",                  "local/IO APIC information", cmd_apic },
	{ "hrtime",  "hrtime",                "high-resolution clock self-test", cmd_hrtime },
	{ "nsleeptest", "nsleeptest",           "nanosecond sleep self-test", cmd_nsleeptest },
	{ "cpus",    "cpus",                  "per-CPU table", cmd_cpus },
	{ "load",    "load",                  "CPU busy percentages", cmd_load },
	{ "top",     "top [seconds]",         "live task and CPU view", cmd_top },
	{ "touch",   "touch <file>",          "create an empty file", cmd_touch },
	{ "pathtest", "pathtest",             "path normalization self-test", cmd_pathtest },
	{ "cd",      "cd [dir]",              "change directory", cmd_cd },
	{ "pwd",     "pwd",                   "print the working directory", cmd_pwd },
	{ "mkdir",   "mkdir <dir>",           "create a directory", cmd_mkdir },
	{ "rmdir",   "rmdir <dir>",           "remove an empty directory", cmd_rmdir },
	{ "rm",      "rm <file>",             "delete a file", cmd_rm },
	{ "format",  "format",                "erase the disk and make a filesystem", cmd_format },
	{ "fstest",  "fstest",                "self-test the filesystem", cmd_fstest },
	{ "pgtest",  "pgtest",                "self-test address spaces", cmd_pgtest },
	{ "shm",     "shm [rm <key>]",        "shared memory segments", cmd_shm },
	{ "backtrace", "backtrace",          "print the current kernel call chain", cmd_backtrace },
	{ "assert",  "assert",                "trip a failing assertion (tests the panic message)", cmd_assert },
	{ "crashdump", "crashdump [show|clear]", "show the crash dump saved by the last panic", cmd_crashdump },
	{ "gdbstub", "gdbstub",               "stop in a GDB remote stub on COM2", cmd_gdbstub },
	{ "pipetest", "pipetest",             "test pipes between two tasks", cmd_pipetest },
	{ "shtest",  "shtest",                "test pipelines and redirection", cmd_shtest },
	{ "jobs",    "jobs",                  "list background jobs", cmd_jobs },
	{ "fg",      "fg [%n]",               "wait for a background job", cmd_fg },
	{ "sh",      "sh <script> [args]",    "run a shell script", cmd_sh },
	{ "let",     "let NAME=expr",         "integer arithmetic on shell variables", cmd_let },
	{ "test",    "test EXPR",             "evaluate a condition (status 0 = true)", cmd_test },
	{ "alias",   "alias [name=text]",     "define or list command aliases", cmd_alias },
	{ "unalias", "unalias name",          "remove an alias", cmd_unalias },
	{ "functions", "functions",           "list shell functions", cmd_functions },
	{ "grep",    "grep [-ivnc] pat [file]", "print lines that contain pat", tu_grep },
	{ "wc",      "wc [-lwc] [file...]",   "count lines, words and bytes", tu_wc },
	{ "head",    "head [-n N] [file]",    "first lines of a file", tu_head },
	{ "tail",    "tail [-n N] [file]",    "last lines of a file", tu_tail },
	{ "sort",    "sort [-nru] [file...]", "sort lines", tu_sort },
	{ "find",    "find [dir] [-name pat]", "search a directory tree", tu_find },
	{ "panic",   "panic [message]",       "deliberately panic (prints a stack trace)", cmd_panic },
	{ "ksym",   "ksym [name|0xADDR]",    "kernel symbol table", cmd_ksym },
	{ "pcache",  "pcache [drop|test]",    "file page cache", cmd_pcache },
	{ "oomtest", "oomtest",               "out-of-memory killer self-test", cmd_oomtest },
	{ "slabinfo", "slabinfo",             "show slab caches", cmd_slabinfo },
	{ "slabtest", "slabtest",             "self-test the slab allocator", cmd_slabtest },
	{ "cowtest", "cowtest",               "copy-on-write self-test", cmd_cowtest },
	{ "disk",  "disk [lba]",            "disk info / dump a sector", cmd_disk },
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

/* The first '|' outside quotes (and not escaped, and not part of '||'), or NULL. */
static char *find_pipe(char *s)
{
	char quote = 0;

	for (; *s; s++) {
		if (quote) {
			if (*s == quote)
				quote = 0;
		} else if (*s == '"' || *s == '\'') {
			quote = *s;
		} else if (s[0] == '$' && s[1] == '(' && s[2] == '(') {
			s = arith_end(s); /* operators inside $(( )) are not shell syntax */
		} else if (*s == '\\' && s[1]) {
			s++;
		} else if (*s == '|') {
			if (s[1] == '|') {
				s++;
				continue;
			}
			return s;
		}
	}
	return 0;
}

#define PIPELINE_CAP 16384

/* ---------------- background jobs ---------------- */

#define MAX_JOBS 8

struct job {
	int used, id;
	uint32_t tid;
	volatile int done, status;
	char cmd[80];
};

static struct job jobs[MAX_JOBS];
static int next_job_id = 1;

static void job_entry(void *arg)
{
	struct job *j = arg;

	j->status = shell_exec(j->cmd);
	j->done = 1;
}

/* 1 (and the command without its '&') if the line ends in an unquoted single '&'. */
static int strip_ampersand(char *line)
{
	char quote = 0;
	char *s, *end = line + kstrlen(line);

	for (s = line; *s; s++) {
		if (quote) {
			if (*s == quote)
				quote = 0;
		} else if (*s == '"' || *s == '\'') {
			quote = *s;
		} else if (s[0] == '$' && s[1] == '(' && s[2] == '(') {
			s = arith_end(s); /* operators inside $(( )) are not shell syntax */
		} else if (*s == '\\' && s[1]) {
			s++;
		}
	}
	while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	if (quote || end == line || end[-1] != '&' || (end - 1 > line && end[-2] == '&'))
		return 0;
	end[-1] = '\0';
	return 1;
}

static int start_job(const char *cmd)
{
	struct job *j = 0;
	int i;

	for (i = 0; i < MAX_JOBS && !j; i++)
		if (!jobs[i].used)
			j = &jobs[i];
	if (!j) {
		console_write("too many background jobs\n");
		return 1;
	}
	if (!kstrncmp(cmd, "run ", 4)) {
		console_write("user programs cannot run in the background yet\n");
		return 1;
	}
	memset(j, 0, sizeof *j);
	kstrlcpy(j->cmd, cmd, sizeof j->cmd);
	j->used = 1;
	j->id = next_job_id++;
	j->tid = task_create("job", job_entry, j, PRIO_DEFAULT)->id;
	console_printf("[%d] %u\n", j->id, j->tid);
	return 0;
}

/* Called before each prompt: report jobs that finished meanwhile. */
static void report_jobs(void)
{
	int i;

	for (i = 0; i < MAX_JOBS; i++) {
		if (jobs[i].used && jobs[i].done) {
			console_printf("[%d]+ Done (%d)  %s\n", jobs[i].id, jobs[i].status, jobs[i].cmd);
			jobs[i].used = 0;
		}
	}
}

/* "%2" or "2" names job 2; no argument means the newest job. */
static struct job *job_find(const char *spec)
{
	struct job *best = 0;
	uint32_t id = 0;
	int i;

	if (spec) {
		if (*spec == '%')
			spec++;
		if (kstrtoul(spec, &id))
			return 0;
	}
	for (i = 0; i < MAX_JOBS; i++) {
		if (!jobs[i].used)
			continue;
		if (spec ? jobs[i].id == (int)id : (!best || jobs[i].id > best->id))
			best = &jobs[i];
	}
	return best;
}

static int job_kill(const char *spec)
{
	struct job *j = job_find(spec);

	if (!j) {
		console_printf("no such job: %s\n", spec);
		return 1;
	}
	if (!j->done && task_kill(j->tid)) {
		console_write("cannot kill that job\n");
		return 1;
	}
	if (!j->done) {
		j->status = -1;
		j->done = 1;
	}
	console_printf("[%d] killed  %s\n", j->id, j->cmd);
	j->used = 0;
	return 0;
}

/* jobs : list the background jobs */
static int cmd_jobs(int argc, char **argv)
{
	int i, n = 0;

	(void)argc;
	(void)argv;
	for (i = 0; i < MAX_JOBS; i++) {
		if (!jobs[i].used)
			continue;
		console_printf("[%d] %-8s task %u  %s\n", jobs[i].id, jobs[i].done ? "Done" : "Running", jobs[i].tid,
			       jobs[i].cmd);
		n++;
	}
	if (!n)
		console_write("no jobs\n");
	return 0;
}

/* fg [%n] : wait for a background job to finish (Ctrl+C leaves it running) */
static int cmd_fg(int argc, char **argv)
{
	struct job *j = job_find(argc > 1 ? argv[1] : 0);
	int status;

	if (!j) {
		console_write("fg: no such job\n");
		return 1;
	}
	console_printf("%s\n", j->cmd);
	while (!j->done && !shell_interrupted())
		task_sleep(20);
	if (!j->done)
		return 130;
	status = j->status;
	j->used = 0;
	return status;
}

static int exec_command(const char *line);

/*
 * "a | b": a runs to completion with its console output captured; b then runs with that text as its
 * standard input. Stages are strictly sequential (like a pipe that always has room), which is all
 * a single-console system needs. The result is b's status.
 */
int shell_exec(const char *line)
{
	char copy[LINE_MAX], aliasbuf[LINE_MAX];
	char *bar, *data;
	int rc, len;

	line = apply_alias(line, aliasbuf, sizeof aliasbuf);
	kstrlcpy(copy, line, sizeof copy);
	if (strip_ampersand(copy))
		return start_job(copy);
	bar = find_pipe(copy);
	if (!bar)
		return exec_command(line);
	*bar = '\0';
	data = kmalloc(PIPELINE_CAP);
	if (!data) {
		console_write("pipe: out of memory\n");
		return 1;
	}
	console_capture_begin(data, PIPELINE_CAP);
	exec_command(copy);
	len = console_capture_end();
	file_stdin_set(data, (uint32_t)len);
	rc = shell_exec(bar + 1);
	file_stdin_clear();
	kfree(data);
	return rc;
}

#define REDIR_CAP 32768

/*
 * Cuts one redirection ("> file", ">> file", "< file") out of a command line, blanking its text so the rest
 * parses normally. 'op' is '>' or '<'; returns 1 and fills file (expanded) / append, or 0 if there is none.
 */
static int take_redirect(char *line, char op, char *file, int size, int *append)
{
	char quote = 0, *s, *start, *end;
	char raw[LINE_MAX];
	int n = 0;

	for (s = line; *s; s++) {
		if (quote) {
			if (*s == quote)
				quote = 0;
			continue;
		}
		if (*s == '"' || *s == '\'') {
			quote = *s;
		} else if (s[0] == '$' && s[1] == '(' && s[2] == '(') {
			s = arith_end(s);
		} else if (*s == '\\' && s[1]) {
			s++;
		} else if (*s == op) {
			break;
		}
	}
	if (!*s)
		return 0;
	start = s++;
	*append = 0;
	if (op == '>' && *s == '>') {
		*append = 1;
		s++;
	}
	while (*s == ' ' || *s == '\t')
		s++;
	end = s;
	while (*end && *end != ' ' && *end != '\t' && *end != '<' && *end != '>' && n < (int)sizeof raw - 1)
		raw[n++] = *end++;
	raw[n] = '\0';
	memset(start, ' ', (size_t)(end - start));
	if (!n)
		return -1; /* operator without a file name */
	expand(raw, file, size);
	return 1;
}

static int exec_plain(const char *line);

/* A command with optional "> file" / ">> file" output redirection; the output is captured, then stored. */
static int exec_output(const char *line)
{
	char copy[LINE_MAX], file[FILE_PATH_MAX];
	char *buf;
	int append, rc, len, got;

	kstrlcpy(copy, line, sizeof copy);
	got = take_redirect(copy, '>', file, sizeof file, &append);
	if (!got)
		return exec_plain(line);
	if (got < 0) {
		console_write("syntax error: missing file name after '>'\n");
		return 2;
	}
	buf = kmalloc(REDIR_CAP);
	if (!buf) {
		console_write("redirect: out of memory\n");
		return 1;
	}
	len = 0;
	if (append && vfs_size(file) > 0) { /* keep what is already there */
		int old = vfs_size(file);

		if (old < REDIR_CAP / 2)
			len = vfs_read(file, buf, (uint32_t)old);
		if (len < 0)
			len = 0;
	}
	console_capture_begin(buf + len, (unsigned)(REDIR_CAP - len));
	rc = exec_plain(copy);
	len += console_capture_end();
	got = vfs_write(file, buf, (uint32_t)len);
	kfree(buf);
	if (got) {
		fs_fail(file, got);
		return 1;
	}
	return rc;
}

/* "cmd < file": the file's contents become the command's standard input. */
static int exec_command(const char *line)
{
	char copy[LINE_MAX], file[FILE_PATH_MAX];
	char *data;
	int got, append, size, rc;

	kstrlcpy(copy, line, sizeof copy);
	got = take_redirect(copy, '<', file, sizeof file, &append);
	if (!got)
		return exec_output(line);
	if (got < 0) {
		console_write("syntax error: missing file name after '<'\n");
		return 2;
	}
	size = vfs_size(file);
	if (size < 0) {
		fs_fail(file, size);
		return 1;
	}
	data = kmalloc((size_t)size + 1);
	if (!data) {
		console_write("redirect: out of memory\n");
		return 1;
	}
	got = size ? vfs_read(file, data, (uint32_t)size) : 0;
	if (got < 0) {
		kfree(data);
		fs_fail(file, got);
		return 1;
	}
	file_stdin_set(data, (uint32_t)got);
	rc = exec_output(copy);
	file_stdin_clear();
	kfree(data);
	return rc;
}

/* "NAME=value" with a valid variable name: the position of '=', or NULL. */
static char *assignment_eq(const char *w)
{
	const char *p = w;

	if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_'))
		return 0;
	while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_' || (*p >= '0' && *p <= '9'))
		p++;
	return *p == '=' ? (char *)p : 0;
}

/* If every word is NAME=value, set them all (as environment variables) and return 1. */
static int all_assignments(int argc, char **argv)
{
	int i;

	for (i = 0; i < argc; i++)
		if (!assignment_eq(argv[i]))
			return 0;
	for (i = 0; i < argc; i++) {
		char *eq = assignment_eq(argv[i]);

		*eq = 0;
		if (env_set(argv[i], eq + 1))
			console_printf("cannot set %s (environment full?)\n", argv[i]);
		*eq = '=';
	}
	return 1;
}

static int exec_plain(const char *line)
{
	char copy[LINE_MAX];
	char *argv[ARGV_MAX];
	const struct command *c;
	int argc;

	expand(line, copy, sizeof copy);
	argc = parse(copy, argv, ARGV_MAX);
	if (argc == 0)
		return 0;

	if (all_assignments(argc, argv)) /* NAME=value ... sets shell variables */
		return last_status = 0;
	if (script_call(argv[0], argc, argv, &last_status)) /* a function defined by a script */
		return last_status;

	cmd_table = commands;
	for (c = commands; c->name; c++) {
		if (!kstrcmp(c->name, argv[0])) {
			last_status = c->fn(argc, argv);
			return last_status;
		}
	}

	{
		char path[VFS_PATH_MAX];

		if (find_external(argv[0], path, sizeof path)) {
			last_status = run_external(path, argc, argv);
			return last_status;
		}
	}

	console_printf("unknown command: %s (try 'help')\n", argv[0]);
	last_status = 127;
	return last_status;
}

int shell_exec_from_user(const char *line)
{
	/* Starting another program from inside one is what the spawn syscall is for. */
	if (!kstrncmp(line, "run ", 4) || !kstrncmp(line, "demo", 4) || !kstrncmp(line, "reboot", 6)
	    || !kstrncmp(line, "halt", 4)) {
		console_write("not allowed from a user program\n");
		return 1;
	}
	return shell_exec(line);
}

/* ---------------- tab completion ---------------- */

#define MAX_CANDIDATES 64

static struct candidate {
	char name[VFS_NAME_MAX + 2];
	int dir;
} cand[MAX_CANDIDATES];
static int ncand;

static void add_candidate(const char *name, const char *prefix, int dir)
{
	size_t n = kstrlen(prefix);

	if (ncand >= MAX_CANDIDATES || kstrncmp(name, prefix, n) || kstrlen(name) >= sizeof cand[0].name - 1)
		return;
	kstrlcpy(cand[ncand].name, name, sizeof cand[0].name);
	cand[ncand++].dir = dir;
}

static void add_files(const char *word)
{
	static struct vfs_dirent ent[FS_MAX_FILES];
	char dir[FILE_PATH_MAX];
	const char *slash = 0, *p, *base;
	int i, n;

	for (p = word; *p; p++)
		if (*p == '/')
			slash = p;
	if (!slash) {
		kstrlcpy(dir, vfs_getcwd(), sizeof dir);
		base = word;
	} else {
		size_t dl = (size_t)(slash - word);

		if (dl >= sizeof dir)
			return;
		memcpy(dir, word, dl);
		dir[dl] = '\0';
		if (!dl)
			kstrlcpy(dir, "/", sizeof dir);
		base = slash + 1;
	}
	n = vfs_list(dir, ent, FS_MAX_FILES);
	for (i = 0; i < n; i++)
		add_candidate(ent[i].name, base, ent[i].is_dir);
}

static void add_commands(const char *word)
{
	const struct command *c;
	const char *n;
	int i;

	for (c = commands; c->name; c++)
		add_candidate(c->name, word, 0);
	for (i = 0; i < MAX_ALIASES; i++)
		if (aliases[i].used)
			add_candidate(aliases[i].name, word, 0);
	for (i = 0; (n = script_function_name(i)); i++)
		add_candidate(n, word, 0);
}

static void type_text(char *buf, int *len, int max, const char *s)
{
	while (*s && *len < max - 1) {
		buf[(*len)++] = *s;
		console_putchar(*s++);
	}
	buf[*len] = '\0';
}

/*
 * Completes the word before the cursor (which is at the end of the line): command names for the first word,
 * file names for the others. One match is finished off; several extend to their common prefix, and a second
 * Tab lists them.
 */
static int complete(char *buf, int *len, int max, int *cur)
{
	static int listed_for = -1;
	char word[64];
	const char *base;
	int ws = *len, first = 1, i, common, n, again;

	while (ws > 0 && buf[ws - 1] != ' ' && buf[ws - 1] != '\t')
		ws--;
	for (i = 0; i < ws; i++)
		if (buf[i] != ' ' && buf[i] != '\t')
			first = 0;
	if (*len - ws >= (int)sizeof word)
		return 0;
	memcpy(word, buf + ws, (size_t)(*len - ws));
	word[*len - ws] = '\0';

	again = listed_for == *len; /* the text is unchanged since the last Tab */
	ncand = 0;
	if (first)
		add_commands(word);
	else
		add_files(word);
	if (!ncand)
		return 0;

	base = word;
	for (i = 0; word[i]; i++)
		if (word[i] == '/')
			base = word + i + 1;
	n = (int)kstrlen(base);

	common = (int)kstrlen(cand[0].name);
	for (i = 1; i < ncand; i++) {
		int k = 0;

		while (k < common && cand[0].name[k] == cand[i].name[k])
			k++;
		common = k;
	}
	if (common > n) { /* there is more that all candidates agree on */
		char add[VFS_NAME_MAX + 2];

		memcpy(add, cand[0].name + n, (size_t)(common - n));
		add[common - n] = '\0';
		type_text(buf, len, max, add);
		n = common;
	}
	if (ncand == 1) {
		type_text(buf, len, max, cand[0].dir ? "/" : " ");
		*cur = *len;
		return 1;
	}
	listed_for = *len;
	*cur = *len;
	if (!again) /* the first Tab only completes; pressing Tab again on the same text lists the choices */
		return 1;
	console_putchar('\n');
	for (i = 0; i < ncand; i++)
		console_printf("%s%s  ", cand[i].name, cand[i].dir ? "/" : "");
	console_putchar('\n');
	prompt();
	console_write(buf);
	*cur = *len;
	return 1;
}

void shell_run(void)
{
	char line[LINE_MAX];

	console_printf("%s %s - type 'help' for commands\n", KERNEL_NAME, KERNEL_VERSION);
	keyboard_set_sigint(sigint);
	env_set("USER", "root");
	env_set("SHELL", "tinysh");
	env_set("TERM", "vga80x25");
	if (is_file("/etc/rc")) { /* startup script */
		char *rc_argv[] = { "/etc/rc", 0 };

		console_write("running /etc/rc\n");
		script_run("/etc/rc", 1, rc_argv);
	}
	for (;;) {
		interrupted = 0;
		report_jobs();
		prompt();
		readline(line, sizeof line);
		hist_add(line);
		shell_exec(line);
	}
}
