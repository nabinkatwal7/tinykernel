#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

#define PRIO_MIN     1
#define PRIO_DEFAULT 5
#define PRIO_MAX     9
#define AGING_TICKS  20 /* every this many waiting ticks a task's effective priority rises by 1 */

typedef enum { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_DEAD, TASK_BLOCKED, TASK_ZOMBIE } task_state_t;

/* Why a task is not runnable; shown by ps. */
typedef enum { WAIT_NONE, WAIT_SLEEP, WAIT_MUTEX, WAIT_SEM, WAIT_KEYBOARD, WAIT_OTHER } wait_reason_t;

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
	uint32_t ubrk, ubrk_min; /* user program break (heap) */
	struct mmap_rec {      /* memory-mapped files of this process */
		uint32_t addr, pages, file_size;
		int shared;
		char path[40];
	} maps[4];
	int is_uproc;          /* a forked user process: it exits through the scheduler, not enter_user() */
	uint32_t heap_bytes;   /* live kmalloc bytes owned by this task */
	uint32_t pgdir;        /* physical address of this task's page directory */
	uint32_t id;           /* process id */
	uint32_t ppid;         /* id of the creating task (0 = kernel main, also for orphans) */
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
	int waitable;          /* becomes a ZOMBIE holding exit_code until task_wait() collects it */
	int exit_code;
	struct task *next;     /* circular list of all tasks */
	struct task *sleep_next; /* wake list, ordered by wake_tick */
	struct task *wait_next;  /* next task in the wait queue we are blocked on */
	struct waitq *wq;        /* queue we are blocked on, if BLOCKED */
	wait_reason_t reason;    /* what we are waiting for while SLEEPING/BLOCKED */
} task_t;

void     sched_init(void);   /* adopts the running code as task 0 and creates the idle task */
task_t  *task_create(const char *name, void (*entry)(void *), void *arg, uint32_t priority);
#define TASKF_WAITABLE 1
task_t  *task_spawn(const char *name, void (*entry)(void *), void *arg, uint32_t priority,
		    uint32_t flags);
void     task_exit_with(int code) __attribute__((noreturn));
int      task_wait(uint32_t id, int *code); /* blocks; 0 = collected, -1 = no such waitable task */
void     task_exit(void) __attribute__((noreturn));
void     task_yield(void);
int      task_fork(void);          /* experimental: child gets 0, parent gets the child id, -1 on error */
void     task_sleep(uint32_t ms);
void     task_sleep_ns(uint64_t ns);   /* sub-tick sleeps spin on the high-resolution clock; longer ones sleep, then spin the remainder */
int      task_kill(uint32_t id);  /* 0 on success */
int      task_set_priority(uint32_t id, uint32_t prio); /* 0 on success */
task_t  *task_current(void);
void     sched_tick(void);

/* Wait queues. wq_wait() must be called with interrupts disabled (irq_save): it blocks the
   current task until another task or IRQ calls wq_wake_*(), then returns still disabled. */
void     wq_wait(struct waitq *q, wait_reason_t why);
int      wq_wake_one(struct waitq *q);   /* 1 if a task was woken */
int      wq_wake_all(struct waitq *q);   /* number woken */        /* called from the timer IRQ */
void     sched_dump(void);
int      sched_format(char *buf, uint32_t cap); /* the same table as text (for /proc/tasks) */
void     sched_tree(void);        /* pstree: tasks indented under their parents */        /* ps */
uint32_t task_count(void);

/* A copy of the interesting task fields, for top-style listings. */
struct task_snapshot {
	uint32_t id, ppid, priority, cpu_ticks, heap_bytes;
	int state;
	char name[16];
};
int      sched_snapshot(struct task_snapshot *out, int max);   /* number of live tasks copied */
uint32_t sched_idle_ticks(void);         /* ticks the boot core spent in the idle task */
uint32_t sched_newest_job(void); /* id of the most recently created user-visible task, 0 if none */
uint32_t sched_current_id(void);
void     sched_account(uint32_t id, int32_t delta); /* adjust a task's heap_bytes */
const char *sched_guard_owner(uint32_t addr); /* task whose guard page contains addr */

/* switch.S */
void switch_context(uint32_t *old_esp, uint32_t new_esp);
int  fork_snapshot(uint32_t *child_esp, uint32_t child_top, uint32_t parent_top);

#endif
