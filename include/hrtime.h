#ifndef HRTIME_H
#define HRTIME_H

#include <stdint.h>

/* High-resolution time from the CPU's time stamp counter, calibrated against the timer tick. */
int      hrtime_init(void);              /* measure the TSC frequency; 0 on success (needs a running tick) */
int      hrtime_available(void);
uint32_t hrtime_khz(void);               /* TSC frequency in kHz */
uint64_t hrtime_ns(void);                /* nanoseconds since hrtime_init() */
uint64_t hrtime_us(void);

#endif
