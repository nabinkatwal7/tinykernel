#include "user.h"

#include "console.h"
#include "elf.h"
#include "file.h"
#include "env.h"
#include "fs.h"
#include "vfs.h"
#include "gdt.h"
#include "idt.h"
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

/* Move the break by delta bytes; returns the old break or (uint32_t)-1. */
static void save_window(uint8_t *buf);
static void restore_window(const uint8_t *buf);

uint32_t user_sbrk(int32_t delta)
{
	task_t *t = task_current();
	uint32_t old = t->ubrk, top = USER_BASE + IMAGE_PAGES * PAGE_SIZE;

	if (delta < 0 ? old - t->ubrk_min < (uint32_t)-delta : delta > 0 && top - old < (uint32_t)delta)
		return (uint32_t)-1;
	t->ubrk += (uint32_t)delta;
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
			return vfs_write(name, builtins[i].start,
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
	uint32_t len = 0, limit = IMAGE_PAGES * PAGE_SIZE;
	unsigned i;
	int n, rc = 0;

	if ((n = vfs_size(name)) >= 0) {
		if (n == 0 || (uint32_t)n > 256 * 1024)
			return -2;
		heap_copy = kmalloc((size_t)n);
		if (!heap_copy)
			return -2;
		if (vfs_read(name, heap_copy, (uint32_t)n) < 0) {
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

	memset(dst, 0, IMAGE_PAGES * PAGE_SIZE); /* only now: a failed lookup must not wipe the caller */
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
static uint32_t push_args(int argc, char **argv)
{
	uint32_t sp = USER_END, ptrs[ARGS_MAX + 1], eptrs[ENV_MAX + 1];
	uint32_t envp_va;
	int i, nenv = env_count();
	char entry[ENV_NAME_MAX + ENV_VAL_MAX + 2];

	for (i = 0; i < nenv; i++) {
		uint32_t len;

		env_entry(i, entry, sizeof entry);
		len = (uint32_t)kstrlen(entry) + 1;
		sp -= len;
		memcpy((void *)sp, entry, len);
		eptrs[i] = sp;
	}
	eptrs[nenv] = 0;

	for (i = 0; i < argc; i++) { /* strings first, last argument at the highest address */
		uint32_t len = (uint32_t)kstrlen(argv[i]) + 1;

		sp -= len;
		memcpy((void *)sp, argv[i], len);
		ptrs[i] = sp;
	}
	sp &= ~3u;
	sp -= 4 * (uint32_t)(nenv + 1);
	memcpy((void *)sp, eptrs, 4 * (uint32_t)(nenv + 1));
	envp_va = sp;
	ptrs[argc] = 0;
	sp -= 4 * (uint32_t)(argc + 1);
	memcpy((void *)sp, ptrs, 4 * (uint32_t)(argc + 1));
	{
		uint32_t argv_va = sp;

		sp -= 4;
		*(uint32_t *)sp = envp_va;
		sp -= 4;
		*(uint32_t *)sp = argv_va;
		sp -= 4;
		*(uint32_t *)sp = (uint32_t)argc;
	}
	return sp;
}

/* Copy the path and argv out of the program's memory (it is about to be overwritten). */
static char stage_path[FS_NAME_MAX];
static char stage_names[ARGS_MAX][64];
static char *stage_argv[ARGS_MAX];
static int stage_argc;

static void stage_args(const char *path, char *const *user_argv)
{
	int i;

	kstrlcpy(stage_path, path, sizeof stage_path);
	stage_argc = 0;
	if (user_argv) {
		for (i = 0; i < ARGS_MAX && user_argv[i]; i++) {
			kstrlcpy(stage_names[i], user_argv[i], sizeof stage_names[i]);
			stage_argv[i] = stage_names[i];
			stage_argc++;
		}
	}
	if (stage_argc == 0) {
		kstrlcpy(stage_names[0], stage_path, sizeof stage_names[0]);
		stage_argv[0] = stage_names[0];
		stage_argc = 1;
	}
}

/*
 * exec(): replace the running program with another one. The new image is loaded into the same
 * window and the interrupt frame is rewritten so that returning from the system call starts it
 * from the top with fresh arguments. Returns -1 (program untouched) if the path is unknown;
 * if the new image turns out to be unloadable the old one is already gone, so it is stopped.
 */
int user_exec(struct regs *r, const char *path, char *const *user_argv)
{
	uint32_t size, entry = USER_BASE, image_end = USER_BASE;
	int rc;

	stage_args(path, user_argv);
	rc = load_image(stage_path, (uint8_t *)USER_BASE, &size, &entry, &image_end);
	if (rc == -1)
		return -1;
	if (rc) {
		console_printf("exec: cannot load %s\n", stage_path);
		user_abort();
	}
	file_close_all();
	task_current()->ubrk_min = task_current()->ubrk = (image_end + 15) & ~15u;
	r->useresp = push_args(stage_argc, stage_argv);
	r->eip = entry;
	r->eax = r->ebx = r->ecx = r->edx = r->esi = r->edi = r->ebp = 0;
	klog(LOG_INFO, "user: exec '%s' (%u bytes)", stage_path, size);
	return 0;
}

extern uint32_t user_saved_esp; /* switch.S: where enter_user()/user_return() meet */

/*
 * spawn(): run another program to completion and return its exit code, then carry on with the
 * caller. There is only one program window, so the caller's whole window is parked in kernel
 * memory while the child runs in it and put back afterwards (registers and kernel stack stay
 * where they are: we are still inside the caller's system call). Returns -1 if the program does
 * not exist or memory ran out.
 */
int user_spawn(const char *path, char *const *user_argv)
{
	uint32_t size, entry = USER_BASE, image_end = USER_BASE;
	uint32_t saved_esp = user_saved_esp, saved_brk = task_current()->ubrk, saved_brk_min = task_current()->ubrk_min;
	uint32_t saved_esp0 = task_current()->esp0;
	uint8_t *backup;
	volatile int marker;
	int rc;

	stage_args(path, user_argv);
	backup = kmalloc((IMAGE_PAGES + STACK_PAGES) * PAGE_SIZE);
	if (!backup)
		return -1;
	save_window(backup);

	rc = load_image(stage_path, (uint8_t *)USER_BASE, &size, &entry, &image_end);
	if (rc) {
		restore_window(backup);
		kfree(backup);
		return rc == -1 ? -1 : -2;
	}
	klog(LOG_INFO, "user: spawn '%s' (%u bytes)", stage_path, size);
	task_current()->ubrk_min = task_current()->ubrk = (image_end + 15) & ~15u;

	task_current()->esp0 = (uint32_t)&marker - 256; /* ring 3 interrupts land below this frame */
	gdt_set_kernel_stack(task_current()->esp0);
	rc = enter_user(entry, push_args(stage_argc, stage_argv));

	task_current()->esp0 = saved_esp0;
	gdt_set_kernel_stack(saved_esp0);
	user_saved_esp = saved_esp;
	restore_window(backup);
	kfree(backup);
	task_current()->ubrk = saved_brk;
	task_current()->ubrk_min = saved_brk_min;
	abort_requested = 0;
	klog(LOG_INFO, "user: spawned '%s' exited with code %d", stage_path, rc);
	return rc;
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
	frames = pmm_alloc_contig(IMAGE_PAGES); /* the stack pages are allocated when first touched */
	udir = paging_new_dir();
	if (!frames || !udir) {
		console_write("out of memory\n");
		goto fail;
	}
	for (i = 0; i < IMAGE_PAGES; i++)
		if (paging_map(udir, USER_BASE + i * PAGE_SIZE, frames + i * PAGE_SIZE,
			       PTE_RW | PTE_US))
			goto fail;
	/* the cloned table still holds the kernel's identity entries for these addresses: remove them so
	   the guard and stack pages really are "not present" (stack pages appear on first touch) */
	for (i = IMAGE_PAGES; i < USER_PAGES; i++)
		paging_unmap(udir, USER_BASE + i * PAGE_SIZE);
	saved_dir = t->pgdir;
	t->pgdir = udir;
	paging_switch(udir);             /* from here on the window is addressed through the user mapping */
	switch (load_image(name, (uint8_t *)USER_BASE, &size, &entry, &image_end)) {
	case 0:
		break;
	case -1:
		console_printf("no such program: %s\n", name);
		goto fail_switched;
	default:
		console_printf("cannot load %s\n", name);
		goto fail_switched;
	}

	klog(LOG_INFO, "user: running '%s' (%u bytes), entry %x", name, size, entry);

	/* Ring 3 interrupts must land below the kernel frames we are standing in. */
	esp0 = (uint32_t)&marker - 256;
	saved_esp0 = t->esp0;
	t->esp0 = esp0;
	gdt_set_kernel_stack(esp0);

	abort_requested = 0;
	file_reset(); /* descriptors 0-2 = console */
	t->ubrk_min = t->ubrk = (image_end + 15) & ~15u;
	active = 1;
	rc = enter_user(entry, push_args(argc > ARGS_MAX ? ARGS_MAX : argc, argv));
	active = 0;
	file_close_all(); /* flush anything the program forgot to close */
	t->pgdir = saved_dir;
	paging_switch(saved_dir);

	t->esp0 = saved_esp0;
	gdt_set_kernel_stack(saved_esp0);
	paging_destroy_user(udir);       /* page tables and every user frame (some may be shared with forked children) */
	klog(LOG_INFO, "user: '%s' exited with code %d", name, rc);
	return rc;

fail_switched:
	t->pgdir = saved_dir;
	paging_switch(saved_dir);
	paging_destroy_user(udir);
	return -1;
fail:
	if (udir)
		paging_free_dir(udir);
	if (frames)
		pmm_free_range(frames, IMAGE_PAGES);
	return -1;
}

void user_exit(int code)
{
	if (task_current()->is_uproc)
		user_proc_exit(code);
	user_return(code);
}

void user_abort(void)
{
	if (task_current()->is_uproc)
		user_proc_exit(-1);
	user_return(-1);
}

/*
 * Demand paging for the stack: the pages at the top of the window are not mapped until the program
 * (or the kernel on its behalf) first touches them. Anything else unmapped - including the guard page
 * under the stack - is a real fault.
 */
int user_demand_fault(uint32_t addr)
{
	task_t *t = task_current();
	uint32_t page = addr & ~(PAGE_SIZE - 1), frame;

	if (!active && !t->is_uproc)
		return 0;
	if (page < USER_END - STACK_RESERVE || page >= USER_END)
		return 0;
	frame = pmm_alloc();
	if (!frame)
		return 0;
	memset((void *)frame, 0, PAGE_SIZE);               /* new memory is always zeroed */
	if (paging_map(0, page, frame, PTE_RW | PTE_US)) {
		pmm_free(frame);
		return 0;
	}
	return 1;
}

/* The window is [image+heap][guard][stack]: the guard page is not mapped, so copy the two parts. */
static void save_window(uint8_t *buf)
{
	memcpy(buf, (void *)USER_BASE, IMAGE_PAGES * PAGE_SIZE);
	memcpy(buf + IMAGE_PAGES * PAGE_SIZE, (void *)(USER_END - STACK_RESERVE), STACK_RESERVE);
}

static void restore_window(const uint8_t *buf)
{
	memcpy((void *)USER_BASE, buf, IMAGE_PAGES * PAGE_SIZE);
	memcpy((void *)(USER_END - STACK_RESERVE), buf + IMAGE_PAGES * PAGE_SIZE, STACK_RESERVE);
}

/* ---- fork ---- */

extern void jump_to_regs(struct regs *r) __attribute__((noreturn)); /* switch.S: resume user mode from a saved frame */

struct fork_start {
	struct regs regs;
	uint32_t dir, brk, brk_min;
};

/* First code of a forked process: adopt the copied address space and "return from the fork syscall". */
static void fork_child_main(void *arg)
{
	struct fork_start *fs = arg;
	struct regs regs = fs->regs;
	task_t *t = task_current();

	t->pgdir = fs->dir;
	t->ubrk = fs->brk;
	t->ubrk_min = fs->brk_min;
	t->is_uproc = 1;
	paging_switch(fs->dir);
	kfree(fs);
	jump_to_regs(&regs); /* eax is already 0: this is the child */
}

/* fork(): returns the child's pid to the caller; the child resumes after the same syscall with 0. */
int user_fork(struct regs *r)
{
	task_t *me = task_current(), *child;
	struct fork_start *fs;
	uint32_t dir;

	if (!active && !me->is_uproc)
		return -1;
	dir = paging_fork_dir(me->pgdir);
	fs = kmalloc(sizeof *fs);
	if (!dir || !fs) {
		if (dir)
			paging_destroy_user(dir);
		kfree(fs);
		return -1;
	}
	fs->regs = *r;
	fs->regs.eax = 0;
	fs->dir = dir;
	fs->brk = me->ubrk;
	fs->brk_min = me->ubrk_min;
	child = task_spawn("uproc", fork_child_main, fs, me->priority, TASKF_WAITABLE);
	if (!child) {
		paging_destroy_user(dir);
		kfree(fs);
		return -1;
	}
	return (int)child->id;
}

/* A forked process ends: release its address space and leave the scheduler with the exit code. */
void user_proc_exit(int code)
{
	task_t *t = task_current();
	uint32_t dir = t->pgdir;

	t->pgdir = paging_kernel_dir();
	paging_switch(t->pgdir);
	paging_destroy_user(dir);
	task_exit_with(code);
}
