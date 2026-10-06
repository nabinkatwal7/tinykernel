#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

#define CLOCK_REALTIME  0   /* wall clock: seconds since 1970 (from the RTC at boot, then advanced) */
#define CLOCK_MONOTONIC 1   /* never jumps; counts from boot */

struct timespec {
	uint32_t tv_sec;
	uint32_t tv_nsec;
};

void     clock_init(void);                      /* remembers the RTC time at boot */
int      clock_gettime(int clock_id, struct timespec *ts);   /* 0, or -1 for an unknown clock */
uint64_t clock_monotonic_ns(void);
uint32_t clock_boot_unix(void);                 /* RTC time at boot, seconds since 1970 */

#endif
