#ifndef SMP_H
#define SMP_H

#include <stdint.h>

/* Bringing up the application processors (the other cores). */
int  smp_start_all(void);               /* starts every present core that is not running; returns how many came up */
int  smp_start_cpu(int cpu_id);         /* 0 if the core reached the kernel */
/* Run fn(arg) on another core (which must be online). smp_post() starts it, smp_wait() waits for the end. */
int  smp_post(int cpu_id, void (*fn)(void *), void *arg);   /* 0 if the job was handed over */
int  smp_wait(int cpu_id);                                   /* 0 once it finished */
int  smp_run_on(int cpu_id, void (*fn)(void *), void *arg);  /* post + wait */
uint32_t smp_heartbeat(int cpu_id);     /* a counter the core increments while alive (proof of life) */

#endif
