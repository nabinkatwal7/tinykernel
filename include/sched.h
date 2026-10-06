#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

typedef enum { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_DEAD } task_state_t;

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
	uint32_t priority;     /* scales the time slice (1 = default) */
	uint32_t slice_left;   /* ticks left in the current quantum */
	uint32_t wake_tick;    /* when SLEEPING */
	uint32_t cpu_ticks;    /* total ticks spent running, for ps */
	void *stack;           /* kmalloc'd stack, NULL for the boot task */
	void (*entry)(void *);
	void *arg;
	int is_idle;
	struct task *next;     /* circular list of all tasks */
	struct task *sleep_next; /* wake list, ordered by wake_tick */
} task_t;

void     sched_init(void);   /* adopts the running code as task 0 and creates the idle task */
task_t  *task_create(const char *name, void (*entry)(void *), void *arg, uint32_t priority);
void     task_exit(void) __attribute__((noreturn));
void     task_yield(void);
void     task_sleep(uint32_t ms);
int      task_kill(uint32_t id);  /* 0 on success */
task_t  *task_current(void);
void     sched_tick(void);        /* called from the timer IRQ */
void     sched_dump(void);        /* ps */
uint32_t task_count(void);
uint32_t sched_current_id(void);
void     sched_account(uint32_t id, int32_t delta); /* adjust a task's heap_bytes */
const char *sched_guard_owner(uint32_t addr); /* task whose guard page contains addr */

/* switch.S */
void switch_context(uint32_t *old_esp, uint32_t new_esp);

#endif
