#include "shell.h"

#include "ata.h"
#include "bcache.h"
#include "console.h"
#include "cpu.h"
#include "debug.h"
#include "editor.h"
#include "env.h"
#include "fat12.h"
#include "fs.h"
#include "gfx.h"
#include "gui.h"
#include "io.h"
#include "keyboard.h"
#include "klog.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "mouse.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "rtc.h"
#include "sched.h"
#include "slab.h"
#include "speaker.h"
#include "sync.h"
#include "timer.h"
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
		if (c == '$' && !single && (in[1] == '?' || in[1] == '_' || (in[1] >= 'A' && in[1] <= 'Z')
					    || (in[1] >= 'a' && in[1] <= 'z'))) {
			char name[ENV_NAME_MAX];
			const char *val;
			int n = 0;

			in++;
			if (*in == '?') {
				in++;
				ksnprintf(name, sizeof name, "%d", last_status);
				val = name;
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

static int recurse(int n)
{
	volatile char pad[256];

	pad[0] = (char)n;
	return recurse(n + 1) + pad[0];
}

static void overflow_main(void *arg)
{
	(void)arg;
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
	char *buf;
	int n;

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
	n = vfs_read(argv[1], buf, (uint32_t)n);
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
	{ "slabinfo", "slabinfo",             "show slab caches", cmd_slabinfo },
	{ "slabtest", "slabtest",             "self-test the slab allocator", cmd_slabtest },
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

int shell_exec(const char *line)
{
	char copy[LINE_MAX];
	char *argv[ARGV_MAX];
	const struct command *c;
	int argc;

	expand(line, copy, sizeof copy);
	argc = parse(copy, argv, ARGV_MAX);
	if (argc == 0)
		return 0;

	cmd_table = commands;
	for (c = commands; c->name; c++) {
		if (!kstrcmp(c->name, argv[0])) {
			last_status = c->fn(argc, argv);
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

void shell_run(void)
{
	char line[LINE_MAX];

	console_printf("%s %s - type 'help' for commands\n", KERNEL_NAME, KERNEL_VERSION);
	keyboard_set_sigint(sigint);
	env_set("USER", "root");
	env_set("SHELL", "tinysh");
	env_set("TERM", "vga80x25");
	for (;;) {
		interrupted = 0;
		prompt();
		readline(line, sizeof line);
		hist_add(line);
		shell_exec(line);
	}
}
