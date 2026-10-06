#ifndef SMPSCHED_H
#define SMPSCHED_H

#include <stdint.h>

#include "percpu.h"

#define SMPT_PER_CPU 8     /* threads one application processor can host */

struct smpt_info {
	int id, cpu, done;
	char name[16];
	uint32_t cpu_ticks;
};

void smpsched_cpu_init(int cpu);            /* an AP calls this once, before enabling its timer */
void smpsched_tick(void);                   /* from the timer interrupt on an application processor */

/* Create a thread on an application processor (cpu < 0: the least loaded one). Returns a thread id or -1.
   The function runs on another core: it must not use the heap, console or any other kernel service. */
int  smpt_create(int cpu, const char *name, void (*fn)(void *), void *arg);
int  smpt_join(int id, uint32_t timeout_ms);   /* 0 once it has finished */
int  smpt_reap(void);                          /* free the stacks of finished threads; returns how many */
int  smpt_info(int cpu, int slot, struct smpt_info *out);
void smpsched_stats(int cpu, uint32_t *busy, uint32_t *idle, uint32_t *switches);

#endif
