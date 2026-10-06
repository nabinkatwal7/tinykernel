#include "console.h"
#include "fs.h"
#include "kstring.h"
#include "net.h"
#include "sched.h"
#include "selftest.h"
#include "shell.h"
#include "timer.h"

/*
 * The test runner: executes the kernel's self-test commands one after another with their output
 * captured, and reports PASS/FAIL per test plus a summary. A test passes when its command returns 0.
 */
struct test {
	const char *name;
	const char *command;
	int needs_fs;
	int needs_net;
};

static const struct test tests[] = {
	{ "path normalization",    "pathtest",            0, 0 },
	{ "ipv4 checksums",        "iptest",              0, 0 },
	{ "allocator stress",      "memtest",             0, 0 },
	{ "heap corruption check", "heapcheck test",      0, 0 },
	{ "slab allocator",        "slabtest",            0, 0 },
	{ "address spaces",        "pgtest",              0, 0 },
	{ "spinlocks",             "spintest",            0, 0 },
	{ "mutex",                 "mutextest",           0, 0 },
	{ "semaphore",             "semtest",             0, 0 },
	{ "task exit codes",       "waittest",            0, 0 },
	{ "producer/consumer",     "prodcons",            0, 0 },
	{ "scheduler priorities",  "priotest",            0, 0 },
	{ "pipes between tasks",   "pipetest",            0, 0 },
	{ "user: strings",         "run strtest",         0, 0 },
	{ "user: printf",          "run printftest",      0, 0 },
	{ "user: malloc",          "run malloctest",      0, 0 },
	{ "user: devices",         "run devtest",         0, 0 },
	{ "user: kernel isolation", "run evil",           0, 0 }, /* must be killed: see below */
	{ "ram disk",              "ramdisk test",        0, 0 },
	{ "loopback device",       "losetup test",        1, 0 },
	{ "partition table",       "fdisk test",          0, 0 },
	{ "ata dma",               "dma test",            0, 0 },
	{ "ext2 read-only driver", "ext2 test",           0, 0 },
	{ "filesystem",            "fstest",              1, 0 },
	{ "fs journal replay",     "jtest",               1, 0 },
	{ "pipes and redirection", "shtest",              1, 0 },
	{ "fsck on a clean disk",  "fsck",                1, 0 },
	{ "user: open/close",      "run opentest",        1, 0 },
	{ "user: read/write",      "run filetest",        1, 0 },
	{ "user: directories",     "run dirtest",         1, 0 },
	{ "user: seek",            "run seektest",        1, 0 },
	{ "user: descriptors",     "run fdtest",          1, 0 },
	{ "user: pipe syscall",    "run pipetest",        0, 0 },
	{ "user: file locks",      "run locktest",        1, 0 },
	{ "icmp loopback",         "icmptest",            0, 1 },
	{ "udp loopback",          "udp test",            0, 1 },
	{ "tx queue",              "txtest",              0, 1 },
};
#define NTESTS (sizeof tests / sizeof tests[0])

static char capture[1024];

/* A user program that is *supposed* to be stopped by the kernel returns -1 from run: invert it. */
static int expect_failure(const struct test *t)
{
	return !kstrcmp(t->command, "run evil");
}

int selftest_run(int verbose)
{
	unsigned i;
	int passed = 0, failed = 0, skipped = 0;
	uint32_t start = timer_ticks();

	console_printf("running %u self-tests%s\n", (unsigned)NTESTS, verbose ? " (verbose)" : "");
	for (i = 0; i < NTESTS && !shell_interrupted(); i++) {
		const struct test *t = &tests[i];
		int rc, ok, len;

		if ((t->needs_fs && !fs_mounted()) || (t->needs_net && !netif.up)) {
			console_printf("  [SKIP] %s (%s)\n", t->name, t->needs_fs && !fs_mounted() ? "no formatted disk" : "no network");
			skipped++;
			continue;
		}
		if (!verbose)
			console_capture_begin(capture, sizeof capture);
		rc = shell_exec(t->command);
		len = verbose ? 0 : console_capture_end();
		ok = expect_failure(t) ? rc != 0 : rc == 0;
		console_printf("  [%s] %s\n", ok ? "PASS" : "FAIL", t->name);
		if (!ok && !verbose && len) { /* show what the failing test said */
			console_write("        ");
			console_write(capture);
			if (capture[len - 1] != '\n')
				console_putchar('\n');
		}
		ok ? passed++ : failed++;
	}
	console_printf("%d passed, %d failed, %d skipped in %u s\n", passed, failed, skipped,
		       (timer_ticks() - start) / timer_hz());
	return failed;
}
