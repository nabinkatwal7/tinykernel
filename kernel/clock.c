#include "clock.h"

#include "hrtime.h"
#include "rtc.h"
#include "timer.h"

static uint32_t boot_unix;
static uint64_t boot_ns_offset;   /* monotonic ns at the moment the RTC was read */

void clock_init(void)
{
	struct rtc_time t;

	rtc_read(&t);
	boot_unix = rtc_unix(&t);
	boot_ns_offset = clock_monotonic_ns();
}

uint32_t clock_boot_unix(void)
{
	return boot_unix;
}

/* High resolution when the TSC is calibrated, otherwise the 10 ms tick. */
uint64_t clock_monotonic_ns(void)
{
	return hrtime_available() ? hrtime_ns() : (uint64_t)timer_ticks() * (1000000000u / timer_hz());
}

int clock_gettime(int id, struct timespec *ts)
{
	uint64_t ns;

	switch (id) {
	case CLOCK_MONOTONIC:
		ns = clock_monotonic_ns();
		break;
	case CLOCK_REALTIME:
		ns = clock_monotonic_ns() - boot_ns_offset + (uint64_t)boot_unix * 1000000000u;
		break;
	default:
		return -1;
	}
	ts->tv_sec = (uint32_t)(ns / 1000000000u);
	ts->tv_nsec = (uint32_t)(ns % 1000000000u);
	return 0;
}
