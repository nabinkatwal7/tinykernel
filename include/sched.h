#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

#define PRIO_MIN     1
#define PRIO_DEFAULT 5
#define PRIO_MAX     9
#define AGING_TICKS  20 /* every this many waiting ticks a task's effective priority rises by 1 */

typedef enum { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_DEAD, TASK_BLOCKED } task_state_t;

struct task;
/* FIFO of tasks blocked on something (mutex, semaphore, keyboard...). */
struct waitq {
	struct task *head, *tail;
};

/* Task control block. */
typedef struct task {
	uint32_t esp;          /* saved kernel stack pointer while not running */
	uint32_t esp0;         /* kernel stack the CPU switches to on ring3 -> ring0 */
	uint32_t guard;        /* physical/virtual address of the unmapped stack guard page */
	uint32_t heap_bytes;   /* live kmalloc bytes owned by this task */
	uint32_t pgdir;        /* physical address of this task's page directory */
	uint32_t id;
	char name[16];
	task_state_t state;
	uint32_t priority;     /* 1 (lowest) .. 9 (highest), default PRIO_DEFAULT */
	uint32_t waited;       /* ticks spent READY without running: raises effective priority */
	uint32_t slice_left;   /* ticks left in the current quantum */
	uint32_t wake_tick;    /* when SLEEPING */
	uint32_t cpu_ticks;    /* total ticks spent running, for ps */
	void *stack;           /* kmalloc'd stack, NULL for the boot task */
	void (*entry)(void *);
	void *arg;
	int is_idle;
	struct task *next;     /* circular list of all tasks */
	struct task *sleep_next; /* wake list, ordered by wake_tick */
	struct task *wait_next;  /* next task in the wait queue we are blocked on */
	struct waitq *wq;        /* queue we are blocked on, if BLOCKED */
} task_t;

void     sched_init(void);   /* adopts the running code as task 0 and creates the idle task */
task_t  *task_create(const char *name, void (*entry)(void *), void *arg, uint32_t priority);
void     task_exit(void) __attribute__((noreturn));
void     task_yield(void);
void     task_sleep(uint32_t ms);
int      task_kill(uint32_t id);  /* 0 on success */
int      task_set_priority(uint32_t id, uint32_t prio); /* 0 on success */
task_t  *task_current(void);
void     sched_tick(void);

/* Wait queues. wq_wait() must be called with interrupts disabled (irq_save): it blocks the
   current task until another task or IRQ calls wq_wake_*(), then returns still disabled. */
void     wq_wait(struct waitq *q);
int      wq_wake_one(struct waitq *q);   /* 1 if a task was woken */
int      wq_wake_all(struct waitq *q);   /* number woken */        /* called from the timer IRQ */
void     sched_dump(void);        /* ps */
uint32_t task_count(void);
uint32_t sched_current_id(void);
void     sched_account(uint32_t id, int32_t delta); /* adjust a task's heap_bytes */
const char *sched_guard_owner(uint32_t addr); /* task whose guard page contains addr */

/* switch.S */
void switch_context(uint32_t *old_esp, uint32_t new_esp);

#endif
