#include "hrtime.h"

#include "cpu.h"
#include "klog.h"
#include "timer.h"

#define SHIFT 22

static uint32_t khz, mult;       /* ns = cycles * mult >> SHIFT, mult = 1e6 * 2^SHIFT / khz */
static uint64_t base_tsc;
static int ok;

int hrtime_init(void)
{
	uint32_t start;
	uint64_t t0, t1;

	if (!(cpu_get()->features_edx & CPU_TSC))
		return -1;
	start = timer_ticks();
	while (timer_ticks() == start) /* begin on a tick edge */
		;
	start = timer_ticks();
	t0 = cpu_rdtsc();
	while (timer_ticks() - start < 20)   /* 200 ms at 100 Hz */
		;
	t1 = cpu_rdtsc();
	khz = (uint32_t)(t1 - t0) / (20u * 1000u / timer_hz());
	if (!khz)
		return -1;
	mult = (uint32_t)(((uint64_t)1000000 << SHIFT) / khz);
	base_tsc = t1;
	ok = 1;
	klog(LOG_INFO, "hrtime: TSC %u kHz (%u.%03u MHz)", khz, khz / 1000, khz % 1000);
	return 0;
}

int hrtime_available(void) { return ok; }
uint32_t hrtime_khz(void) { return khz; }

uint64_t hrtime_ns(void)
{
	uint64_t cycles, lo, hi;

	if (!ok)
		return (uint64_t)timer_ticks() * (1000000000u / timer_hz()); /* tick resolution fallback */
	cycles = cpu_rdtsc() - base_tsc;
	hi = cycles >> 32;
	lo = cycles & 0xFFFFFFFFu;
	/* split the 64x32 multiply so it cannot overflow for a very long uptime */
	return ((hi * mult) << (32 - SHIFT)) + ((lo * mult) >> SHIFT);
}

uint64_t hrtime_us(void)
{
	return hrtime_ns() / 1000;
}
