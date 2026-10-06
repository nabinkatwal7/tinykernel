#ifndef SMP_H
#define SMP_H

#include <stdint.h>

/* Bringing up the application processors (the other cores). */
int  smp_start_all(void);               /* starts every present core that is not running; returns how many came up */
int  smp_start_cpu(int cpu_id);         /* 0 if the core reached the kernel */
uint32_t smp_heartbeat(int cpu_id);     /* a counter the core increments while alive (proof of life) */

#endif
