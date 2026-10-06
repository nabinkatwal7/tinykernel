#ifndef CPUSTAT_H
#define CPUSTAT_H

#include <stdint.h>

/*
 * CPU load statistics. Once a second a kernel timer snapshots, per core, how many ticks the core
 * was busy and how many it was idle; the history (the last 60 seconds) answers "how busy was
 * this core over the last N seconds".
 */
#define CPUSTAT_HISTORY 60

void cpustat_init(void);                                  /* start the one-second sampler */
/* Busy percentage (0-100) of a core over the last 'seconds' seconds (clamped to the history). */
uint32_t cpustat_busy_percent(int cpu, uint32_t seconds);
uint32_t cpustat_samples(void);                           /* how many one-second samples exist so far */

#endif
