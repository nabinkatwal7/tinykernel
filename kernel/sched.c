#include "sched.h"

#include "console.h"
#include "gdt.h"
#include "io.h"
#include "klog.h"
#include "kprintf.h"
#include "kmalloc.h"
#include "kstring.h"
#include "paging.h"
#include "pmm.h"
#include "slab.h"
#include "timer.h"

#define TASK_STACK_SIZE 8192
#define BASE_SLICE      5 /* ticks per quantum */

static task_t *task_head;   /* circular list */
static task_t *current;
static task_t *idle;
static uint32_t next_id;
static task_t *sleep_head; /* sleeping tasks, earliest wake_tick first */

/* Insert keeping the list sorted; equal wake times keep FIFO order. */
static void sleep_insert(task_t *t)
{
	task_t **pp = &sleep_head;

	while (*pp && (int32_t)((*pp)->wake_tick - t->wake_tick) <= 0)
		pp = &(*pp)->sleep_next;
	t->sleep_next = *pp;
	*pp = t;
}

static void sleep_remove(task_t *t)
{
	task_t **pp = &sleep_head;

	while (*pp && *pp != t)
		pp = &(*pp)->sleep_next;
	if (*pp)
		*pp = t->sleep_next;
	t->sleep_next = 0;
}
static struct slab_cache task_cache;

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
	task_t *t;

	slab_cache_init(&task_cache, "task_t", sizeof(task_t));
	t = slab_alloc(&task_cache);

	kstrlcpy(t->name, "kmain", sizeof t->name);
	t->pgdir = paging_kernel_dir();
	t->id = next_id++;
	t->state = TASK_RUNNING;
	t->priority = PRIO_DEFAULT;
	t->slice_left = BASE_SLICE;
	t->esp0 = 0x90000; /* the boot stack top; only used if a ring 3 task starts here */
	list_append(t);
	current = t;

	idle = task_create("idle", idle_main, 0, 1);
	idle->is_idle = 1;
	klog(LOG_INFO, "sched: round-robin, %u tick quantum, %u Hz", BASE_SLICE, timer_hz());
}

/* Allocate a TCB plus a guarded stack; the caller fills in the initial frame and links it. */
static task_t *task_alloc(const char *name, uint32_t priority)
{
	task_t *t = slab_alloc(&task_cache);

	if (!t)
		return 0;
	/* [guard page][stack]: the guard is unmapped so an overflow faults instead of corrupting. */
	t->guard = pmm_alloc_contig(1 + TASK_STACK_SIZE / PAGE_SIZE);
	t->stack = t->guard ? (void *)(t->guard + PAGE_SIZE) : 0;
	if (!t->stack) {
		slab_free(&task_cache, t);
		return 0;
	}
	paging_unmap(paging_kernel_dir(), t->guard);
	kstrlcpy(t->name, name, sizeof t->name);
	t->esp0 = (uint32_t)t->stack + TASK_STACK_SIZE;
	t->ppid = current ? current->id : 0;
	t->pgdir = paging_kernel_dir();
	t->priority = priority < PRIO_MIN ? PRIO_MIN : priority > PRIO_MAX ? PRIO_MAX : priority;
	return t;
}

static void task_publish(task_t *t)
{
	uint32_t flags = irq_save();

	t->id = next_id++;
	t->state = TASK_READY;
	list_append(t);
	irq_restore(flags);
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg, uint32_t priority)
{
	task_t *t = task_alloc(name, priority);
	uint32_t *sp;

	if (!t)
		return 0;
	sp = (uint32_t *)((uint8_t *)t->stack + TASK_STACK_SIZE);
	*--sp = 0;                          /* fake return address for task_trampoline */
	*--sp = (uint32_t)task_trampoline;  /* popped by the ret in switch_context */
	*--sp = 0;                          /* ebp */
	*--sp = 0;                          /* ebx */
	*--sp = 0;                          /* esi */
	*--sp = 0;                          /* edi */
	t->esp = (uint32_t)sp;
	t->entry = entry;
	t->arg = arg;
	task_publish(t);
	return t;
}

/*
 * Fork experiment: duplicate the calling task's kernel stack so the child resumes right after
 * this call with return value 0 (the parent gets the child's id). The copy is only correct
 * if pointers into the old stack are fixed up, which we do conservatively: any word in the
 * copy that falls inside the parent's stack range is shifted into the child's stack. That
 * covers saved frame pointers and addresses of locals but can mis-relocate an integer that
 * happens to look like a stack address.
 */
int task_fork(void)
{
	uint32_t f = irq_save();
	task_t *p = current, *c;
	uint32_t ptop, ctop, delta, *w;

	if (!p->stack) { /* the boot task's stack has no safe bottom to return to */
		irq_restore(f);
		return -1;
	}
	c = task_alloc(p->name, p->priority);
	if (!c) {
		irq_restore(f);
		return -1;
	}
	ptop = (uint32_t)p->stack + TASK_STACK_SIZE;
	ctop = (uint32_t)c->stack + TASK_STACK_SIZE;

	if (fork_snapshot(&c->esp, ctop, ptop)) {
		delta = ctop - ptop;
		for (w = (uint32_t *)c->esp; w < (uint32_t *)ctop; w++)
			if (*w >= (uint32_t)p->stack && *w <= ptop)
				*w += delta;
		c->entry = p->entry;
		c->arg = p->arg;
		task_publish(c);
		irq_restore(f);
		return (int)c->id;
	}
	irq_restore(f); /* child: 'f' is the copy of the parent's saved flags */
	return 0;
}

static int runnable(task_t *t)
{
	return !t->is_idle && (t->state == TASK_READY || t->state == TASK_RUNNING);
}

/* Effective priority = base priority + one level per AGING_TICKS spent waiting. */
static uint32_t effective(task_t *t)
{
	return t->priority + t->waited / AGING_TICKS;
}

/*
 * Highest effective priority wins. The ring is scanned starting after the current task and only a
 * strictly better task replaces the best one, so equal priorities still rotate round-robin, and
 * aging guarantees a low-priority task eventually overtakes a busy high-priority one.
 */
static task_t *pick_next(void)
{
	task_t *t = current->next, *best = 0;

	do {
		if (runnable(t) && (!best || effective(t) > effective(best)))
			best = t;
		t = t->next;
	} while (t != current->next);
	if (best)
		best->waited = 0;
	return best ? best : idle;
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
		paging_map(paging_kernel_dir(), t->guard, t->guard, PTE_RW);
		pmm_free_range(t->guard, 1 + TASK_STACK_SIZE / PAGE_SIZE);
		slab_free(&task_cache, t);
	}
}

/* A dying task's children are adopted by task 0, like orphans going to init. */
static void reparent_children(task_t *dead)
{
	task_t *t = task_head;

	do {
		if (t->ppid == dead->id && t != dead)
			t->ppid = 0;
		t = t->next;
	} while (t != task_head);
}

static void wq_remove(task_t *t);
static void schedule_locked(void)
{
	task_t *prev = current, *next = pick_next();

	if (next != prev) {
		if (prev->state == TASK_RUNNING)
			prev->state = TASK_READY;
		next->state = TASK_RUNNING;
		next->slice_left = BASE_SLICE;
		current = next;
		gdt_set_kernel_stack(next->esp0);
		paging_switch(next->pgdir);
		switch_context(&prev->esp, next->esp);
	} else {
		prev->state = TASK_RUNNING;
		prev->slice_left = BASE_SLICE;
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
	current->reason = WAIT_SLEEP;
	sleep_insert(current);
	schedule_locked();
	irq_restore(f);
}

void task_exit(void)
{
	cli();
	current->state = TASK_DEAD;
	reparent_children(current);
	schedule_locked();
	for (;;)
		hlt();
}

int task_set_priority(uint32_t id, uint32_t prio)
{
	task_t *t = task_head;
	uint32_t f = irq_save();
	int rc = -1;

	if (prio >= PRIO_MIN && prio <= PRIO_MAX) {
		do {
			if (t->id == id && !t->is_idle && t->state != TASK_DEAD) {
				t->priority = prio;
				rc = 0;
				break;
			}
			t = t->next;
		} while (t != task_head);
	}
	irq_restore(f);
	return rc;
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
			if (t->state == TASK_SLEEPING)
				sleep_remove(t);
			if (t->state == TASK_BLOCKED)
				wq_remove(t);
			t->state = TASK_DEAD;
			reparent_children(t);
			rc = 0;
			break;
		}
		t = t->next;
	} while (t != task_head);
	irq_restore(f);
	return rc;
}

const char *sched_guard_owner(uint32_t addr)
{
	task_t *t = task_head;

	if (!t)
		return 0;
	do {
		if (t->guard && addr >= t->guard && addr < t->guard + PAGE_SIZE)
			return t->name;
		t = t->next;
	} while (t != task_head);
	return 0;
}

uint32_t sched_current_id(void)
{
	return current ? current->id : 0;
}

void sched_account(uint32_t id, int32_t delta)
{
	task_t *t = task_head;

	if (!t)
		return;
	do {
		if (t->id == id) {
			t->heap_bytes += (uint32_t)delta;
			return;
		}
		t = t->next;
	} while (t != task_head);
}

void wq_wait(struct waitq *q, wait_reason_t why)
{
	current->reason = why;
	current->state = TASK_BLOCKED;
	current->wq = q;
	current->wait_next = 0;
	if (q->tail)
		q->tail->wait_next = current;
	else
		q->head = current;
	q->tail = current;
	schedule_locked();
}

int wq_wake_one(struct waitq *q)
{
	uint32_t f = irq_save();
	task_t *t = q->head;

	if (t) {
		q->head = t->wait_next;
		if (!q->head)
			q->tail = 0;
		t->wait_next = 0;
		t->wq = 0;
		t->state = TASK_READY;
		t->reason = WAIT_NONE;
	}
	irq_restore(f);
	return t != 0;
}

int wq_wake_all(struct waitq *q)
{
	int n = 0;

	while (wq_wake_one(q))
		n++;
	return n;
}

/* Take a blocked task out of its wait queue (used when it is killed). */
static void wq_remove(task_t *t)
{
	struct waitq *q = t->wq;
	task_t **pp, *prev = 0;

	if (!q)
		return;
	for (pp = &q->head; *pp && *pp != t; prev = *pp, pp = &(*pp)->wait_next)
		;
	if (*pp) {
		*pp = t->wait_next;
		if (q->tail == t)
			q->tail = prev;
	}
	t->wq = 0;
	t->wait_next = 0;
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
	uint32_t now = timer_ticks();

	if (!current)
		return;
	while (sleep_head && (int32_t)(now - sleep_head->wake_tick) >= 0) {
		task_t *t = sleep_head;

		sleep_head = t->sleep_next;
		t->sleep_next = 0;
		t->state = TASK_READY;
		t->reason = WAIT_NONE;
	}

	{
		task_t *t = task_head;

		do { /* everyone who wanted the CPU this tick but did not get it ages */
			if (runnable(t) && t != current)
				t->waited++;
			t = t->next;
		} while (t != task_head);
	}
	current->cpu_ticks++;
	if (current->is_idle || current->slice_left == 0 || --current->slice_left == 0)
		schedule_locked();
}

void sched_dump(void)
{
	static const char *const names[] = { "ready", "running", "sleeping", "dead", "blocked" };
	static const char *const why[] = { "", "", "mutex", "sem", "kbd", "other" };
	uint32_t f = irq_save();
	task_t *t = task_head;
	char state[24];

	console_write("ID   PPID NAME        STATE           PRIO  CPU-TICKS  HEAP    STACK\n");
	do {
		if (t->state != TASK_DEAD) {
			if (t->state == TASK_BLOCKED)
				ksnprintf(state, sizeof state, "blocked:%s", why[t->reason]);
			else
				ksnprintf(state, sizeof state, "%s", names[t->state]);
			console_printf("%-4u %-4u %-11s %-15s %-5u %-10u %-7u %u\n", t->id, t->ppid, t->name,
				       state, t->priority, t->cpu_ticks, t->heap_bytes,
				       t->stack ? TASK_STACK_SIZE : 0u);
		}
		t = t->next;
	} while (t != task_head);
	irq_restore(f);
}

static void tree_print(task_t *node, int depth)
{
	task_t *t = task_head;
	int i;

	for (i = 0; i < depth; i++)
		console_write("  ");
	console_printf("%s%s (%u)\n", depth ? "`- " : "", node->name, node->id);
	do {
		if (t->ppid == node->id && t != node && t->state != TASK_DEAD && !t->is_idle)
			tree_print(t, depth + 1);
		t = t->next;
	} while (t != task_head);
}

void sched_tree(void)
{
	uint32_t f = irq_save();
	task_t *t = task_head;

	do { /* roots: task 0, the idle task and anything whose parent is gone */
		if (t->state != TASK_DEAD && (t->id == 0 || t->is_idle))
			tree_print(t, 0);
		t = t->next;
	} while (t != task_head);
	irq_restore(f);
}
