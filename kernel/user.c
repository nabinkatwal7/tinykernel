#include "user.h"

#include "console.h"
#include "elf.h"
#include "file.h"
#include "env.h"
#include "fs.h"
#include "gdt.h"
#include "io.h"
#include "klog.h"
#include "kmalloc.h"
#include "kmalloc.h"
#include "kstring.h"
#include "paging.h"
#include "pmm.h"
#include "sched.h"

#define builtins builtin_progs
#define NBUILTIN builtin_nprogs

static int active;
static volatile int abort_requested;
static uint32_t brk, brk_min; /* program break: the heap lives between the image and the stack */

/* Move the break by delta bytes; returns the old break or (uint32_t)-1. */
uint32_t user_sbrk(int32_t delta)
{
	uint32_t old = brk, top = USER_END - STACK_RESERVE;

	if (delta < 0 ? old - brk_min < (uint32_t)-delta : delta > 0 && top - old < (uint32_t)delta)
		return (uint32_t)-1;
	brk += (uint32_t)delta;
	return old;
}

void user_request_abort(void)
{
	abort_requested = 1;
}

int user_abort_requested(void)
{
	return abort_requested;
}

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

#define ARGS_MAX 16

/*
 * Find the program (disk first, then built-ins), then place it in the program window: ELF files
 * are loaded segment by segment, anything else is treated as a flat binary linked at USER_BASE.
 * Returns 0 and the entry point, or a negative value (-1 not found, -2 bad/too big image).
 */
static int load_image(const char *name, uint8_t *dst, uint32_t *size, uint32_t *entry,
		      uint32_t *image_end)
{
	const uint8_t *img = 0;
	uint8_t *heap_copy = 0;
	uint32_t len = 0, limit = USER_PAGES * PAGE_SIZE - STACK_RESERVE;
	unsigned i;
	int n, rc = 0;

	memset(dst, 0, USER_PAGES * PAGE_SIZE);

	if (fs_mounted() && (n = fs_size(name)) >= 0) {
		if (n == 0 || (uint32_t)n > 256 * 1024)
			return -2;
		heap_copy = kmalloc((size_t)n);
		if (!heap_copy)
			return -2;
		if (fs_read(name, heap_copy, (uint32_t)n) < 0) {
			kfree(heap_copy);
			return -2;
		}
		img = heap_copy;
		len = (uint32_t)n;
	}
	for (i = 0; !img && i < NBUILTIN; i++) {
		if (!kstrcmp(builtins[i].name, name)) {
			img = builtins[i].start;
			len = (uint32_t)(builtins[i].end - builtins[i].start);
		}
	}
	if (!img)
		return -1;

	if (elf_is_elf(img, len)) {
		rc = elf_load(img, len, dst, USER_BASE, limit, entry, image_end);
		if (rc)
			console_printf("bad ELF image (error %d)\n", rc);
	} else if (len > limit) {
		rc = -2;
	} else {
		memcpy(dst, img, len);
		*entry = USER_BASE;
		*image_end = USER_BASE + len;
	}
	*size = len;
	kfree(heap_copy);
	return rc ? -2 : 0;
}

/*
 * Lay out the initial user stack (top down): argument strings, environment strings, the argv and
 * envp pointer arrays (NULL terminated), then envp, argv, argc - so [esp]=argc, [esp+4]=argv,
 * [esp+8]=envp. Returns the initial user esp.
 */
static uint32_t push_args(uint32_t frames, int argc, char **argv)
{
	uint32_t sp = USER_END, ptrs[ARGS_MAX + 1], eptrs[ENV_MAX + 1], base = frames - USER_BASE;
	uint32_t envp_va;
	int i, nenv = env_count();
	char entry[ENV_NAME_MAX + ENV_VAL_MAX + 2];

	for (i = 0; i < nenv; i++) {
		uint32_t len;

		env_entry(i, entry, sizeof entry);
		len = (uint32_t)kstrlen(entry) + 1;
		sp -= len;
		memcpy((void *)(base + sp), entry, len);
		eptrs[i] = sp;
	}
	eptrs[nenv] = 0;

	for (i = 0; i < argc; i++) { /* strings first, last argument at the highest address */
		uint32_t len = (uint32_t)kstrlen(argv[i]) + 1;

		sp -= len;
		memcpy((void *)(base + sp), argv[i], len);
		ptrs[i] = sp;
	}
	sp &= ~3u;
	sp -= 4 * (uint32_t)(nenv + 1);
	memcpy((void *)(base + sp), eptrs, 4 * (uint32_t)(nenv + 1));
	envp_va = sp;
	ptrs[argc] = 0;
	sp -= 4 * (uint32_t)(argc + 1);
	memcpy((void *)(base + sp), ptrs, 4 * (uint32_t)(argc + 1));
	{
		uint32_t argv_va = sp;

		sp -= 4;
		*(uint32_t *)(base + sp) = envp_va;
		sp -= 4;
		*(uint32_t *)(base + sp) = argv_va;
		sp -= 4;
		*(uint32_t *)(base + sp) = (uint32_t)argc;
	}
	return sp;
}

int user_run(const char *name)
{
	char *argv[1] = { (char *)name };

	return user_run_args(name, 1, argv);
}

int user_run_args(const char *name, int argc, char **argv)
{
	uint32_t size, entry = USER_BASE, image_end = USER_BASE, esp0, saved_esp0, frames = 0, udir = 0, saved_dir, i;
	volatile int marker;
	task_t *t = task_current();
	int rc;

	if (active) {
		console_write("a user program is already running\n");
		return -1;
	}
	/* Private frames + a private address space: nothing else can see or touch them. */
	frames = pmm_alloc_contig(USER_PAGES);
	udir = paging_new_dir();
	if (!frames || !udir) {
		console_write("out of memory\n");
		goto fail;
	}
	for (i = 0; i < USER_PAGES; i++)
		if (paging_map(udir, USER_BASE + i * PAGE_SIZE, frames + i * PAGE_SIZE,
			       PTE_RW | PTE_US))
			goto fail;
	switch (load_image(name, (uint8_t *)frames, &size, &entry, &image_end)) { /* kernel writes via the identity map */
	case 0:
		break;
	case -1:
		console_printf("no such program: %s\n", name);
		goto fail;
	default:
		console_printf("cannot load %s\n", name);
		goto fail;
	}

	klog(LOG_INFO, "user: running '%s' (%u bytes), entry %x", name, size, entry);

	/* Ring 3 interrupts must land below the kernel frames we are standing in. */
	esp0 = (uint32_t)&marker - 256;
	saved_esp0 = t->esp0;
	t->esp0 = esp0;
	gdt_set_kernel_stack(esp0);

	saved_dir = t->pgdir;
	t->pgdir = udir;
	paging_switch(udir);
	abort_requested = 0;
	brk_min = brk = (image_end + 15) & ~15u;
	active = 1;
	rc = enter_user(entry, push_args(frames, argc > ARGS_MAX ? ARGS_MAX : argc, argv));
	active = 0;
	file_close_all(); /* flush anything the program forgot to close */
	t->pgdir = saved_dir;
	paging_switch(saved_dir);

	t->esp0 = saved_esp0;
	gdt_set_kernel_stack(saved_esp0);
	paging_free_dir(udir);
	pmm_free_range(frames, USER_PAGES);
	klog(LOG_INFO, "user: '%s' exited with code %d", name, rc);
	return rc;

fail:
	if (udir)
		paging_free_dir(udir);
	if (frames)
		pmm_free_range(frames, USER_PAGES);
	return -1;
}

void user_exit(int code)
{
	user_return(code);
}

void user_abort(void)
{
	user_return(-1);
}
