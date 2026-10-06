#include "user.h"

#include "console.h"
#include "fs.h"
#include "gdt.h"
#include "io.h"
#include "klog.h"
#include "kmalloc.h"
#include "kstring.h"
#include "pmm.h"
#include "sched.h"

/* Embedded by builtin.S */
extern const uint8_t builtin_hello_start[], builtin_hello_end[];
extern const uint8_t builtin_counter_start[], builtin_counter_end[];
extern const uint8_t builtin_fault_start[], builtin_fault_end[];
extern const uint8_t builtin_evil_start[], builtin_evil_end[];

struct builtin {
	const char *name;
	const uint8_t *start, *end;
};

static const struct builtin builtins[] = {
	{ "hello",   builtin_hello_start,   builtin_hello_end },
	{ "counter", builtin_counter_start, builtin_counter_end },
	{ "fault",   builtin_fault_start,   builtin_fault_end },
	{ "evil",    builtin_evil_start,    builtin_evil_end },
};
#define NBUILTIN (sizeof builtins / sizeof builtins[0])

static int active;

int user_is_active(void)
{
	return active;
}

void user_list_builtin(void)
{
	unsigned i;

	for (i = 0; i < NBUILTIN; i++)
		console_printf("  %-8s %u bytes\n", builtins[i].name,
			       (uint32_t)(builtins[i].end - builtins[i].start));
}

int user_install_builtin(const char *name)
{
	unsigned i;

	for (i = 0; i < NBUILTIN; i++)
		if (!kstrcmp(builtins[i].name, name))
			return fs_write(name, builtins[i].start,
					(uint32_t)(builtins[i].end - builtins[i].start));
	return FS_ENOENT;
}

/* Copy a program image to USER_BASE. Looks on disk first, then in the built-in table. */
static int load_image(const char *name, uint32_t *size)
{
	uint8_t *dst = (uint8_t *)USER_BASE;
	unsigned i;
	int n;

	memset(dst, 0, USER_PAGES * PAGE_SIZE);

	if (fs_mounted()) {
		n = fs_size(name);
		if (n >= 0) {
			if ((uint32_t)n > USER_PAGES * PAGE_SIZE / 2)
				return -1;
			n = fs_read(name, dst, (uint32_t)n);
			if (n >= 0) {
				*size = (uint32_t)n;
				return 0;
			}
			return -1;
		}
	}
	for (i = 0; i < NBUILTIN; i++) {
		if (!kstrcmp(builtins[i].name, name)) {
			*size = (uint32_t)(builtins[i].end - builtins[i].start);
			memcpy(dst, builtins[i].start, *size);
			return 0;
		}
	}
	return -1;
}

int user_run(const char *name)
{
	uint32_t size, esp0, saved_esp0;
	volatile int marker;
	task_t *t = task_current();
	int rc;

	if (active) {
		console_write("a user program is already running\n");
		return -1;
	}
	if (pmm_reserve(USER_BASE, USER_PAGES)) {
		console_write("cannot reserve user memory (is there 9 MiB of RAM?)\n");
		return -1;
	}
	if (load_image(name, &size)) {
		pmm_free_range(USER_BASE, USER_PAGES);
		console_printf("no such program: %s\n", name);
		return -1;
	}

	klog(LOG_INFO, "user: running '%s' (%u bytes) at %x", name, size, USER_BASE);

	/* Ring 3 interrupts must land below the kernel frames we are standing in. */
	esp0 = (uint32_t)&marker - 256;
	saved_esp0 = t->esp0;
	t->esp0 = esp0;
	gdt_set_kernel_stack(esp0);

	active = 1;
	rc = enter_user(USER_BASE, USER_END - 16);
	active = 0;

	t->esp0 = saved_esp0;
	gdt_set_kernel_stack(saved_esp0);
	pmm_free_range(USER_BASE, USER_PAGES);
	klog(LOG_INFO, "user: '%s' exited with code %d", name, rc);
	return rc;
}

void user_exit(int code)
{
	user_return(code);
}

void user_abort(void)
{
	user_return(-1);
}
