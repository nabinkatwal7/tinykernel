#include "smpsched.h"

#include "apic.h"
#include "io.h"
#include "klog.h"
#include "kstring.h"
#include "percpu.h"
#include "pmm.h"
#include "sched.h"
#include "sync.h"

/*
 * Per-core schedulers for the application processors. Each AP owns a run queue of "SMP threads"
 * and preempts among them on its own LAPIC timer tick, so cores never contend for a global run
 * queue (the usual per-CPU design). The boot core keeps the full scheduler in sched.c.
 *
 * SMP threads run on a core that shares the kernel's memory with the boot core but not its
 * locking (those locks only disable interrupts on one core), so such a thread must stay away from
 * the heap, the console and other kernel services: it computes and leaves results in memory.
 */
#define STACK_PAGES 4

enum { T_FREE, T_READY, T_DONE };

struct sthread {
	int id;
	char name[16];
	volatile int state;
	uint32_t esp;
	uint32_t stack;           /* physical = virtual address of the stack block, freed by the boot core */
	void (*fn)(void *);
	void *arg;
	uint32_t cpu_ticks;
};

struct rq {
	struct sthread idle;      /* stands for the core's original context */
	struct sthread thr[SMPT_PER_CPU];
	struct sthread *volatile cur;
	int cur_index;            /* index into thr[] of the running thread, -1 for idle */
	uint32_t busy_ticks, idle_ticks, switches;
	ticketlock_t lock;        /* serialises thread creation */
	int ready;                /* run queue initialised */
};

static struct rq rqs[MAX_CPUS];
static volatile int next_thread_id = 1;

static struct rq *my_rq(void)
{
	return &rqs[this_cpu()->id];
}

/* First code of every SMP thread, entered through the 'ret' of switch_context in IRQ context. */
static void thread_entry(void)
{
	struct sthread *t = my_rq()->cur;

	sti(); /* we arrived from a timer interrupt with interrupts off */
	t->fn(t->arg);
	t->state = T_DONE;
	for (;;) /* never scheduled again: the next tick moves this core elsewhere */
		hlt();
}

/* Timer tick on an application processor: pick the next ready thread, round robin. */
void smpsched_tick(void)
{
	struct rq *q = my_rq();
	struct sthread *prev = q->cur, *next = 0;
	int i, start = q->cur_index, idx = -1;

	this_cpu()->timer_irqs++;
	if (!q->ready)
		return;
	if (prev == &q->idle)
		q->idle_ticks++;
	else {
		q->busy_ticks++;
		prev->cpu_ticks++;
	}

	for (i = 1; i <= SMPT_PER_CPU; i++) {
		int k = (start + i + SMPT_PER_CPU) % SMPT_PER_CPU; /* start may be -1 (idle) */

		if (q->thr[k].state == T_READY) {
			next = &q->thr[k];
			idx = k;
			break;
		}
	}
	if (!next) {
		next = &q->idle;
		idx = -1;
	}
	if (next == prev)
		return;
	q->cur = next;
	q->cur_index = idx;
	q->switches++;
	switch_context(&prev->esp, next->esp);
}

void smpsched_cpu_init(int cpu)
{
	struct rq *q = &rqs[cpu];

	memset(q, 0, sizeof *q);
	q->cur = &q->idle;
	q->cur_index = -1;
	kstrlcpy(q->idle.name, "idle", sizeof q->idle.name);
	q->ready = 1;
}

int smpt_create(int cpu, const char *name, void (*fn)(void *), void *arg)
{
	return cpu < 0 ? smpt_create_affinity(~0u, name, fn, arg) : smpt_create_affinity(1u << cpu, name, fn, arg);
}

int smpt_create_affinity(uint32_t mask, const char *name, void (*fn)(void *), void *arg)
{
	int cpu = -1;
	struct rq *q;
	int i, best = -1;
	uint32_t stack, *sp;

	{ /* the least loaded online application processor that the affinity mask allows */
		int load, best_load = 1 << 30, c;

		for (c = 0; c < percpu_count(); c++) {
			struct percpu *p = percpu_get(c);

			if (!(mask & (1u << c)) || !p->online || p->is_bsp)
				continue;
			for (load = 0, i = 0; i < SMPT_PER_CPU; i++)
				load += rqs[c].thr[i].state == T_READY;
			if (load < best_load) {
				best_load = load;
				best = c;
			}
		}
		cpu = best;
	}
	if (cpu < 0 || cpu >= MAX_CPUS || !percpu_get(cpu) || !percpu_get(cpu)->online || percpu_get(cpu)->is_bsp)
		return -1;
	q = &rqs[cpu];
	if (!q->ready)
		return -1;
	stack = pmm_alloc_contig(STACK_PAGES);
	if (!stack)
		return -1;

	ticket_lock(&q->lock);
	for (i = 0; i < SMPT_PER_CPU && q->thr[i].state != T_FREE; i++)
		;
	if (i == SMPT_PER_CPU) {
		ticket_unlock(&q->lock);
		pmm_free_range(stack, STACK_PAGES);
		return -1;
	}
	{
		struct sthread *t = &q->thr[i];

		t->id = next_thread_id++;
		kstrlcpy(t->name, name, sizeof t->name);
		t->stack = stack;
		t->fn = fn;
		t->arg = arg;
		t->cpu_ticks = 0;
		sp = (uint32_t *)(stack + STACK_PAGES * PAGE_SIZE - 16);
		*--sp = 0;                           /* fake return address for thread_entry */
		*--sp = (uint32_t)thread_entry;      /* popped by the ret in switch_context */
		*--sp = 0;                           /* ebp */
		*--sp = 0;                           /* ebx */
		*--sp = 0;                           /* esi */
		*--sp = 0;                           /* edi */
		t->esp = (uint32_t)sp;
		__asm__ volatile ("" : : : "memory");
		t->state = T_READY;                  /* published last: the core may pick it up at once */
		i = t->id;
	}
	ticket_unlock(&q->lock);
	return i;
}

static struct sthread *find(int id, int *cpu);
static struct sthread *find(int id, int *cpu)
{
	int c, i;

	for (c = 0; c < MAX_CPUS; c++)
		for (i = 0; i < SMPT_PER_CPU; i++)
			if (rqs[c].thr[i].state != T_FREE && rqs[c].thr[i].id == id) {
				*cpu = c;
				return &rqs[c].thr[i];
			}
	return 0;
}

int smpt_cpu_of(int id)
{
	int cpu;

	return find(id, &cpu) ? cpu : -1;
}

int smpt_join(int id, uint32_t timeout_ms)
{
	int cpu;
	struct sthread *t = find(id, &cpu);
	uint32_t waited;

	if (!t)
		return -1;
	for (waited = 0; t->state != T_DONE; waited += 5) {
		if (waited >= timeout_ms)
			return -1;
		task_sleep(5);
	}
	return 0;
}

int smpt_reap(void)
{
	int c, i, n = 0;

	for (c = 0; c < MAX_CPUS; c++) {
		for (i = 0; i < SMPT_PER_CPU; i++) {
			struct sthread *t = &rqs[c].thr[i];

			if (t->state == T_DONE && &rqs[c].thr[i] != rqs[c].cur) {
				pmm_free_range(t->stack, STACK_PAGES);
				t->stack = 0;
				t->state = T_FREE;
				n++;
			}
		}
	}
	return n;
}

int smpt_info(int cpu, int slot, struct smpt_info *out)
{
	struct sthread *t;

	if (cpu < 0 || cpu >= MAX_CPUS || slot < 0 || slot >= SMPT_PER_CPU)
		return -1;
	t = &rqs[cpu].thr[slot];
	if (t->state == T_FREE)
		return -1;
	out->id = t->id;
	out->cpu = cpu;
	kstrlcpy(out->name, t->name, sizeof out->name);
	out->done = t->state == T_DONE;
	out->cpu_ticks = t->cpu_ticks;
	return 0;
}

void smpsched_stats(int cpu, uint32_t *busy, uint32_t *idle, uint32_t *switches)
{
	*busy = rqs[cpu].busy_ticks;
	*idle = rqs[cpu].idle_ticks;
	*switches = rqs[cpu].switches;
}
