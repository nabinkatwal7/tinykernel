#include "sched.h"

#include "console.h"
#include "gdt.h"
#include "io.h"
#include "klog.h"
#include "kmalloc.h"
#include "kstring.h"
#include "paging.h"
#include "timer.h"

#define TASK_STACK_SIZE 8192
#define BASE_SLICE      5 /* ticks per quantum at priority 1 */

static task_t *task_head;   /* circular list */
static task_t *current;
static task_t *idle;
static uint32_t next_id;

static void idle_main(void *arg)
{
	(void)arg;
	for (;;)
		hlt();
}

/* First code a new task runs (reached via the 'ret' in switch_context). */
static void task_trampoline(void)
{
	task_t *t = current;

	sti(); /* we got here from inside a timer IRQ with interrupts off */
	t->entry(t->arg);
	task_exit();
}

static void list_append(task_t *t)
{
	if (!task_head) {
		task_head = t;
		t->next = t;
		return;
	}
	t->next = task_head->next;
	task_head->next = t;
	task_head = t; /* head is just "some task"; the ring order is what matters */
}

void sched_init(void)
{
	task_t *t = kcalloc(1, sizeof *t);

	kstrlcpy(t->name, "kmain", sizeof t->name);
	t->pgdir = paging_kernel_dir();
	t->id = next_id++;
	t->state = TASK_RUNNING;
	t->priority = 1;
	t->slice_left = BASE_SLICE;
	t->esp0 = 0x90000; /* the boot stack top; only used if a ring 3 task starts here */
	list_append(t);
	current = t;

	idle = task_create("idle", idle_main, 0, 1);
	idle->is_idle = 1;
	klog(LOG_INFO, "sched: round-robin, %u tick quantum, %u Hz", BASE_SLICE, timer_hz());
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg, uint32_t priority)
{
	task_t *t = kcalloc(1, sizeof *t);
	uint32_t *sp;
	uint32_t flags;

	if (!t)
		return 0;
	t->stack = kmalloc(TASK_STACK_SIZE);
	if (!t->stack) {
		kfree(t);
		return 0;
	}
	sp = (uint32_t *)((uint8_t *)t->stack + TASK_STACK_SIZE);
	*--sp = 0;                          /* fake return address for task_trampoline */
	*--sp = (uint32_t)task_trampoline;  /* popped by the ret in switch_context */
	*--sp = 0;                          /* ebp */
	*--sp = 0;                          /* ebx */
	*--sp = 0;                          /* esi */
	*--sp = 0;                          /* edi */

	kstrlcpy(t->name, name, sizeof t->name);
	t->esp = (uint32_t)sp;
	t->esp0 = (uint32_t)t->stack + TASK_STACK_SIZE;
	t->pgdir = paging_kernel_dir();
	t->entry = entry;
	t->arg = arg;
	t->priority = priority ? priority : 1;
	t->state = TASK_READY;

	flags = irq_save();
	t->id = next_id++;
	list_append(t);
	irq_restore(flags);
	return t;
}

static int runnable(task_t *t)
{
	return !t->is_idle && (t->state == TASK_READY || t->state == TASK_RUNNING);
}

/* Round-robin: scan the ring starting after the current task so everyone gets a turn. */
static task_t *pick_next(void)
{
	task_t *t = current->next;

	do {
		if (runnable(t))
			return t;
		t = t->next;
	} while (t != current->next);
	return idle;
}

/* Free tasks that have exited. Never touches the one we are running on. */
static void reap(void)
{
	task_t *t, *prev;

	for (;;) {
		prev = current;
		for (t = current->next; t != current; prev = t, t = t->next)
			if (t->state == TASK_DEAD)
				break;
		if (t == current)
			return;
		prev->next = t->next;
		if (task_head == t)
			task_head = prev;
		kfree(t->stack);
		kfree(t);
	}
}

static void schedule_locked(void)
{
	task_t *prev = current, *next = pick_next();

	if (next != prev) {
		if (prev->state == TASK_RUNNING)
			prev->state = TASK_READY;
		next->state = TASK_RUNNING;
		next->slice_left = BASE_SLICE * next->priority;
		current = next;
		gdt_set_kernel_stack(next->esp0);
		paging_switch(next->pgdir);
		switch_context(&prev->esp, next->esp);
	} else {
		prev->state = TASK_RUNNING;
		prev->slice_left = BASE_SLICE * prev->priority;
	}
	reap();
}

void task_yield(void)
{
	uint32_t f = irq_save();

	schedule_locked();
	irq_restore(f);
}

void task_sleep(uint32_t ms)
{
	uint32_t ticks = ms * timer_hz() / 1000;
	uint32_t f = irq_save();

	if (ticks == 0)
		ticks = 1;
	current->wake_tick = timer_ticks() + ticks;
	current->state = TASK_SLEEPING;
	schedule_locked();
	irq_restore(f);
}

void task_exit(void)
{
	cli();
	current->state = TASK_DEAD;
	schedule_locked();
	for (;;)
		hlt();
}

int task_kill(uint32_t id)
{
	task_t *t = task_head;
	uint32_t f = irq_save();
	int rc = -1;

	do {
		if (t->id == id && id != 0 && !t->is_idle && t->state != TASK_DEAD) {
			if (t == current) {
				irq_restore(f);
				task_exit();
			}
			t->state = TASK_DEAD;
			rc = 0;
			break;
		}
		t = t->next;
	} while (t != task_head);
	irq_restore(f);
	return rc;
}

task_t *task_current(void)
{
	return current;
}

uint32_t task_count(void)
{
	uint32_t n = 0, f = irq_save();
	task_t *t = task_head;

	do {
		if (t->state != TASK_DEAD)
			n++;
		t = t->next;
	} while (t != task_head);
	irq_restore(f);
	return n;
}

/* Called from the timer IRQ with interrupts off. */
void sched_tick(void)
{
	task_t *t = task_head;
	uint32_t now = timer_ticks();

	if (!current)
		return;
	do {
		if (t->state == TASK_SLEEPING && (int32_t)(now - t->wake_tick) >= 0)
			t->state = TASK_READY;
		t = t->next;
	} while (t != task_head);

	current->cpu_ticks++;
	if (current->is_idle || current->slice_left == 0 || --current->slice_left == 0)
		schedule_locked();
}

void sched_dump(void)
{
	static const char *const names[] = { "ready", "running", "sleeping", "dead" };
	uint32_t f = irq_save();
	task_t *t = task_head;

	console_write("ID   NAME        STATE     PRIO  CPU-TICKS\n");
	do {
		if (t->state != TASK_DEAD)
			console_printf("%-4u %-11s %-9s %-5u %u\n", t->id, t->name, names[t->state],
				       t->priority, t->cpu_ticks);
		t = t->next;
	} while (t != task_head);
	irq_restore(f);
}
